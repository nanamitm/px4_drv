/*
 * BonDriver が、抜去された機器の受信機から挿し直した機器の受信機へつなぎ直すことを検証する
 * DriverHost の制御とストリームのサーバーを同じプロセスで動かし、TS を出す模擬の受信機を登録する
 * PX4_ISOLATED_TEST_HOST で IPC 名を分離し、動作中の DriverHost_PX4 と接続しない
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <windows.h>

#include "bon_driver.hpp"
#include "ctrl_server.hpp"
#include "ipc_names.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"
#include "security_attributes.hpp"
#include "stream_server.hpp"
#include "util.hpp"

namespace {

void Require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

/* 選局を受け付けて、先頭のペイロードに識別用の値を入れた TS を出し続ける受信機 */
class FakeReceiver final : public px4::ReceiverBase {
public:
	explicit FakeReceiver(std::uint8_t marker) : ReceiverBase(0), marker_(marker) {}
	~FakeReceiver() override { StopWriter(); }

	int Open() override
	{
		opens++;
		open = true;
		return 0;
	}

	void Close() override
	{
		StopWriter();
		stream_buf_->Stop();
		open = false;
	}

	int CheckLock(bool &locked) override
	{
		if (failing)
			return -EIO;
		locked = true;
		return 0;
	}

	int SetLnbVoltage(std::int32_t) override { return 0; }

	int SetCapture(bool capture) override
	{
		if (!capture) {
			StopWriter();
			stream_buf_->Stop();
			return 0;
		}
		if (failing)
			return -EIO;
		StopWriter();
		stream_buf_->Alloc(188 * 1024);
		stream_buf_->SetThresholdSize(188 * 16);
		stream_buf_->Start();
		running_ = true;
		writer_ = std::thread([this] { Write(); });
		return 0;
	}

	int ReadStat(px4::command::StatType, std::int32_t &value) override
	{
		if (failing)
			return -EIO;
		value = 20000;
		return 0;
	}

	/* 抜去を再現する。以後の USB の操作は失敗し、TS も止まる */
	void Unplug()
	{
		failing = true;
		StopWriter();
	}

	std::atomic_uint opens{0};
	std::atomic_uint tunes{0};
	std::atomic_uint32_t last_freq{0};
	std::atomic_bool open{false};
	std::atomic_bool failing{false};

protected:
	int SetFrequency() override
	{
		if (failing)
			return -EIO;
		tunes++;
		last_freq = params_.freq;
		return 0;
	}

	int SetStreamId() override { return 0; }

private:
	void Write()
	{
		std::uint8_t packets[188 * 16] = {};
		for (std::size_t offset = 0; offset < sizeof(packets); offset += 188) {
			packets[offset] = 0x47;
			packets[offset + 1] = 0x01;
			packets[offset + 3] = 0x10;
			packets[offset + 4] = marker_;
		}
		while (running_) {
			std::size_t size = sizeof(packets);
			stream_buf_->Write(packets, size);
			stream_buf_->NotifyWrite();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	void StopWriter()
	{
		running_ = false;
		if (writer_.joinable())
			writer_.join();
	}

	const std::uint8_t marker_;
	std::atomic_bool running_{false};
	std::thread writer_;
};

constexpr wchar_t DeviceName[] = L"Test Device";
const px4::DeviceIdentity Identity{ L"\\\\?\\usb#test-device", L"0123456789" };

void Register(px4::ReceiverManager &manager, FakeReceiver &receiver)
{
	px4::command::ReceiverInfo info{};
	wcscpy_s(info.device_name, DeviceName);
	wcscpy_s(info.receiver_name, L"Test Receiver");
	info.systems = px4::SystemType::ISDB_T;
	info.index = 0;
	Require(manager.Register(info, &receiver, Identity), "register receiver");
}

/* 指定した受信機の TS が BonDriver から届くまで読む */
bool ReadMarker(px4::BonDriver &bon, std::uint8_t marker)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline) {
		bon.WaitTsStream(200);
		BYTE *data = nullptr;
		DWORD size = 0, remain = 0;
		while (bon.GetTsStream(&data, &size, &remain) && size) {
			for (DWORD offset = 0; offset + 188 <= size; offset += 188) {
				if (data[offset] == 0x47 && data[offset + 4] == marker)
					return true;
			}
		}
	}
	return false;
}

/* BonDriver の INI と ChSet を、試験の実行ファイルと同じ名前で置く */
void WriteSettings()
{
	wchar_t path[MAX_PATH];
	Require(GetModuleFileNameW(nullptr, path, MAX_PATH) != 0, "module path");
	std::wstring base = path;
	base.erase(base.rfind(L'.'));

	std::ofstream chset(base + L".ChSet.txt", std::ios::binary | std::ios::trunc);
	chset << "$UHF\t0\r\n13ch\t0\t0\t63\t0\r\n14ch\t0\t1\t64\t0\r\n";
	chset.close();

	/* ChSetPath の相対パスは実行ファイルのフォルダーを基準に解決されるため、ファイル名だけを書く */
	std::ofstream ini(base + L".ini", std::ios::binary | std::ios::trunc);
	ini << "[BonDriver]\r\nName=\"Test\"\r\nSystem=\"ISDB-T\"\r\n"
		"DriverHostPath=\".\\no_driver_host.exe\"\r\nPipeConnectTimeout=2000\r\nTuneTimeout=2000\r\n"
		"[BonDriver.ISDB-T]\r\nChSetPath=\"bondriver_reconnect_test.ChSet.txt\"\r\n"
		"[ReceiverDefinition0]\r\nDeviceName=\"Test Device\"\r\nSystem=\"ISDB-T\"\r\n";
	ini.close();
}

/* 物理チャンネル 63 と 64 は UHF の 13ch と 14ch */
constexpr std::uint32_t Ch13Freq = 95143 + 63 * 6000;
constexpr std::uint32_t Ch14Freq = 95143 + 64 * 6000;

} // namespace

int main() try
{
	/* DLL では DllMain が呼ぶ初期化。INI と ChSet を実行ファイルのフォルダーから読む */
	Require(px4::util::path::Init(GetModuleHandleW(nullptr)), "module path init");
	WriteSettings();

	px4::ReceiverManager manager;
	FakeReceiver first(0xa1);
	Register(manager, first);

	px4::CtrlServer ctrl_server(manager);
	px4::StreamServer stream_server(manager);
	Require(ctrl_server.Start() && stream_server.Start(), "servers did not start");

	/* BonDriver は起動通知のイベントが既にあれば DriverHost を起動せずに接続する */
	HANDLE startup;
	{
		px4::SecurityAttributes sa(EVENT_ALL_ACCESS);
		startup = CreateEventW(sa.Get(), TRUE, TRUE, px4::ipc::Startup);
	}
	Require(startup != nullptr, "startup event");

	{
		px4::BonDriver bon;
		Require(bon.Init(), "BonDriver init");
		Require(bon.OpenTuner(), "open tuner");
		Require(bon.SetChannel(0, 0) && first.last_freq == Ch13Freq, "tune the first receiver");
		Require(ReadMarker(bon, 0xa1), "no TS from the first receiver");

		/* 抜去と挿し直し。信号レベルの取得でつなぎ直し、同じチャンネルを選局し直す */
		first.Unplug();
		manager.MarkRemoved(Identity.path);
		FakeReceiver second(0xb2);
		Register(manager, second);
		bon.GetSignalLevel();
		Require(second.opens == 1 && second.last_freq == Ch13Freq, "signal level did not reconnect and retune");
		Require(!first.open, "the removed receiver was left open");
		Require(bon.GetCurChannel() == 0, "the current channel changed after reconnect");
		Require(ReadMarker(bon, 0xb2), "no TS from the replugged receiver");

		/* 抜去でない失敗ではつなぎ直さない */
		FakeReceiver spare(0xc3);
		Register(manager, spare);
		second.failing = true;
		bon.GetSignalLevel();
		Require(spare.opens == 0, "a plain failure triggered a reconnect");
		second.failing = false;

		/* 選局では試行の間隔を待たずにつなぎ直し、要求したチャンネルを選局する */
		second.Unplug();
		manager.MarkRemoved(Identity.path);
		FakeReceiver third(0xd4);
		Register(manager, third);
		Require(bon.SetChannel(0, 1), "SetChannel did not reconnect");
		Require(third.opens == 1 && third.last_freq == Ch14Freq && spare.opens == 0,
			"SetChannel did not move to the replugged receiver");
		Require(ReadMarker(bon, 0xd4), "no TS after reconnect from SetChannel");

		bon.CloseTuner();
		Require(!third.open, "close tuner left the receiver open");

		manager.Unregister(&first);
		manager.Unregister(&second);
		manager.Unregister(&spare);
		manager.Unregister(&third);
	}

	CloseHandle(startup);
	std::puts("bondriver_reconnect_test: PASS");
	return 0;
}
catch (const std::exception &error) {
	std::fprintf(stderr, "bondriver_reconnect_test: %s\n", error.what());
	return 1;
}
