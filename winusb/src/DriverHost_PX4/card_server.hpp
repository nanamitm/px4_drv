#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "card_command.hpp"
#include "card_reader_directory.hpp"
#include "server_base.hpp"
#include "smart_card.hpp"

namespace px4 {

class CardServer final : public ServerBase {
public:
	CardServer(CardReaderDirectory &card_readers, ReceiverManager &receiver_manager) noexcept;

private:
	struct SharedCard final {
		explicit SharedCard(std::shared_ptr<CardDevice> source_device)
			: device(std::move(source_device)), card(device) {}

		std::shared_ptr<CardDevice> device;
		SmartCard card;
		std::recursive_mutex io_mutex;
		/* io_mutex を保持中の処理が、短い I/O か接続をまたぐトランザクションかを STATUS が区別する */
		std::atomic_bool transaction_active{false};
		std::size_t shared_connections = 0;
		bool exclusive_connection = false;
	};

	class CardConnection final : public ServerBase::Connection {
	public:
		CardConnection(CardServer &parent, std::unique_ptr<PipeServer> &pipe) noexcept;

	private:
		/* 診断ログの集計窓。長時間受信でログが肥大しないよう、成功した APDU は窓ごとにまとめる */
		struct TransmitStats final {
			std::uint64_t count = 0;
			std::uint64_t failures = 0;
			std::uint64_t slow = 0;
			std::uint64_t total_us = 0;
			std::uint64_t max_us = 0;
			std::uint64_t max_wait_us = 0;
			SmartCard::Diagnostics baseline;
			SmartCard::Diagnostics latest;
			bool has_baseline = false;
			std::chrono::steady_clock::time_point window_start;
		};

		void Worker() noexcept override;
		void SetError(card_command::Command &command, int error) const noexcept;
		void ReleaseCard() noexcept;
		void LogTransmit(int ret, const std::uint8_t header[4], std::size_t send_length,
			const card_command::Command &command, std::size_t recv_length,
			std::uint64_t io_us, std::uint64_t wait_us,
			const SmartCard::Diagnostics &before,
			const SmartCard::Diagnostics &after) noexcept;
		void LogTransmitSummary(const char *reason) noexcept;
		void LogStatus(const std::wstring &reader, int ret,
			const card_command::Command &command) noexcept;

		CardServer &card_parent_;
		std::shared_ptr<SharedCard> card_;
		std::unordered_map<std::wstring, std::shared_ptr<SharedCard>> monitored_cards_;
		card_command::ShareMode share_mode_;
		bool transaction_;
		std::uint64_t generation_ = 0;
		std::wstring reader_;
		TransmitStats stats_;
		/* 状態監視は短い周期で繰り返されるため、変化した時だけ記録する */
		std::unordered_map<std::wstring, std::string> last_status_;
	};

	Connection* CreateConnection(std::unique_ptr<PipeServer> &pipe) override;
	std::shared_ptr<SharedCard> GetCard(const std::wstring &reader_name, int &error);
	std::shared_ptr<SharedCard> AcquireCard(const std::wstring &reader_name,
		card_command::ShareMode share_mode, int &error);
	void ReleaseCard(const std::shared_ptr<SharedCard> &card,
		card_command::ShareMode share_mode) noexcept;
	int ChangeShareMode(const std::shared_ptr<SharedCard> &card,
		card_command::ShareMode current_mode,
		card_command::ShareMode requested_mode) noexcept;

	CardReaderDirectory &card_readers_;
	std::mutex cards_mutex_;
	std::unordered_map<std::wstring, std::weak_ptr<SharedCard>> cards_;
};

} // namespace px4
