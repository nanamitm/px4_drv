/*
 * カードサーバーの RECONNECT が、同じリーダー名で挿し直した機器へ接続を移すことを、
 * 実際の名前付きパイプと模擬のカード機器で検証する
 * PX4_ISOLATED_TEST_HOST で IPC 名を分離し、動作中の DriverHost_PX4 と接続しない
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include "card_command.hpp"
#include "card_reader_directory.hpp"
#include "card_server.hpp"
#include "ipc_names.hpp"
#include "pipe_client.hpp"
#include "receiver_manager.hpp"
#include "mock_card_device.hpp"

namespace {

void Require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

/* 抜去後は USB の操作がすべて -ENODEV になる機器を、模擬カードの外側で再現する */
class RemovableCard final : public px4::CardDevice {
public:
	RemovableCard()
	{
		card.is_present = true;
	}

	int OpenCard() override { return removed ? -ENODEV : card.OpenCard(); }
	void CloseCard() override { if (!removed) card.CloseCard(); }
	int DetectCard(bool &detected) override { return removed ? -ENODEV : card.DetectCard(detected); }
	int ResetCard() override { return removed ? -ENODEV : card.ResetCard(); }
	int SetCardBaudrate(std::uint32_t baudrate) override
	{
		return removed ? -ENODEV : card.SetCardBaudrate(baudrate);
	}
	int IsCardDataReady(bool &ready) override { return removed ? -ENODEV : card.IsCardDataReady(ready); }
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override
	{
		return removed ? -ENODEV : card.ReadCardData(buf, len);
	}
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override
	{
		return removed ? -ENODEV : card.WriteCardData(buf, len);
	}

	std::size_t ApduCount() const { return card.apdu_block_lengths.size(); }

	MockCardDevice card;
	std::atomic_bool removed{false};
};

/* DeviceManager の代わりに、抜き差しで機器のオブジェクトを差し替える一覧 */
class Directory final : public px4::CardReaderDirectory {
public:
	std::vector<std::wstring> ListCardReaders() const override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<std::wstring> names;
		for (const auto &entry : devices_)
			names.push_back(entry.first);
		return names;
	}

	std::shared_ptr<px4::CardDevice> FindCardReader(const std::wstring &name) const override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = devices_.find(name);
		return it == devices_.end() ? nullptr : it->second;
	}

	std::uint64_t GetCardReaderGeneration() const noexcept override { return generation_; }

	void Set(const std::wstring &name, std::shared_ptr<px4::CardDevice> device)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		devices_[name] = std::move(device);
		generation_++;
	}

	void Remove(const std::wstring &name)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		devices_.erase(name);
		generation_++;
	}

private:
	mutable std::mutex mutex_;
	std::map<std::wstring, std::shared_ptr<px4::CardDevice>> devices_;
	std::atomic<std::uint64_t> generation_{1};
};

class Client final {
public:
	Client()
	{
		px4::PipeClient::PipeClientConfig config = {};
		config.stream_read = false;
		config.timeout = 5000;
		Require(pipe_.Connect(px4::ipc::Card, config, nullptr), "cannot connect to the card server");
	}

	px4::card_command::Command Call(px4::card_command::Command command)
	{
		command.status = px4::card_command::Status::REQUEST;
		command.error = px4::card_command::Error::NONE;
		Require(pipe_.Call(&command, sizeof(command)), "card pipe call failed");
		return command;
	}

	px4::card_command::Command Simple(px4::card_command::Code code, const wchar_t *reader = nullptr,
		px4::card_command::ShareMode share = px4::card_command::ShareMode::SHARED)
	{
		auto command = std::make_unique<px4::card_command::Command>();
		*command = {};
		command->code = code;
		command->share_mode = static_cast<std::uint32_t>(share);
		if (reader)
			wcscpy_s(command->reader_name, reader);
		return Call(*command);
	}

	px4::card_command::Command Transmit()
	{
		auto command = std::make_unique<px4::card_command::Command>();
		*command = {};
		command->code = px4::card_command::Code::TRANSMIT;
		const std::uint8_t apdu[] = { 0x90, 0x30, 0x00, 0x00, 0x00 };
		std::memcpy(command->data, apdu, sizeof(apdu));
		command->data_length = sizeof(apdu);
		command->data_capacity = px4::card_command::MAX_DATA_SIZE;
		return Call(*command);
	}

private:
	px4::PipeClient pipe_;
};

bool Succeeded(const px4::card_command::Command &command)
{
	return command.status == px4::card_command::Status::SUCCEEDED;
}

bool FailedWith(const px4::card_command::Command &command, px4::card_command::Error error)
{
	return command.status == px4::card_command::Status::FAILED && command.error == error;
}

} // namespace

int main() try
{
	using px4::card_command::Code;
	using px4::card_command::Error;
	using px4::card_command::ShareMode;

	const wchar_t *reader = L"Test Smart Card Reader (ID: 0123456789ABCDEF)";
	Directory directory;
	auto first = std::make_shared<RemovableCard>();
	directory.Set(reader, first);

	px4::ReceiverManager receiver_manager;
	px4::CardServer server(directory, receiver_manager);
	Require(server.Start(), "card server did not start");

	Client client;
	Require(Succeeded(client.Simple(Code::CONNECT, reader)), "connect before removal failed");
	Require(Succeeded(client.Transmit()) && first->ApduCount() == 1, "transmit before removal failed");

	/* 同じ機器では、従来どおり共有モードの変更として扱う */
	Require(Succeeded(client.Simple(Code::RECONNECT, nullptr, ShareMode::EXCLUSIVE)),
		"reconnect to the same device failed");
	Require(Succeeded(client.Simple(Code::RECONNECT, nullptr, ShareMode::SHARED)),
		"reconnect back to shared failed");

	/* 抜去の前に始めたトランザクションは、挿し直した機器へ持ち越さない */
	Require(Succeeded(client.Simple(Code::BEGIN_TRANSACTION)), "begin transaction failed");

	first->removed = true;
	directory.Remove(reader);
	Require(!Succeeded(client.Transmit()), "transmit on the removed device succeeded");
	Require(FailedWith(client.Simple(Code::RECONNECT), Error::REMOVED),
		"reconnect while the device is absent did not report removal");

	auto second = std::make_shared<RemovableCard>();
	directory.Set(reader, second);
	Require(Succeeded(client.Simple(Code::RECONNECT)), "reconnect after replug failed");
	Require(Succeeded(client.Transmit()) && second->ApduCount() == 1 && first->ApduCount() == 1,
		"transmit after reconnect did not reach the replugged device");
	Require(Succeeded(client.Simple(Code::BEGIN_TRANSACTION)),
		"the transaction on the removed device was carried over");
	Require(Succeeded(client.Simple(Code::END_TRANSACTION)), "end transaction failed");

	/* 共有数は新しい機器へ移り、切断で解放される */
	Client other;
	Require(FailedWith(other.Simple(Code::CONNECT, reader, ShareMode::EXCLUSIVE), Error::BUSY),
		"exclusive connect ignored the moved shared connection");
	Require(Succeeded(client.Simple(Code::DISCONNECT)), "disconnect failed");
	Require(Succeeded(other.Simple(Code::CONNECT, reader, ShareMode::EXCLUSIVE)),
		"exclusive connect after disconnect failed");
	Require(Succeeded(other.Simple(Code::DISCONNECT)), "second disconnect failed");

	std::puts("card_server_reconnect_test: PASS");
	return 0;
}
catch (const std::exception &error) {
	std::fprintf(stderr, "card_server_reconnect_test: %s\n", error.what());
	return 1;
}
