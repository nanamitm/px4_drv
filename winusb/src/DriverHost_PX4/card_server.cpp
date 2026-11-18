#define msg_prefix	"card"

#include "card_server.hpp"
#include "ipc_names.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "card_command.hpp"
#include "card_status_lock.hpp"
#include "msg.h"

namespace px4 {

namespace {

/*
 * 成功した APDU を個別に記録する処理時間の下限。通常の応答は約70msで返り、
 * ブロックの待機時間の既定値 500ms を超えた要求は再送か WTX を経ている
 */
constexpr std::uint64_t SLOW_TRANSMIT_US = 500000;
/* 数日間の受信でもログを数MBに抑えつつ、ドロップの時刻と照合できる粒度で集計する */
constexpr auto SUMMARY_INTERVAL = std::chrono::seconds(60);

bool CardLogEnabled() noexcept
{
	/* 通常運用ではログファイルを開かないため、文字列の組み立ても省く */
	return (msg_get_mode() & MSG_MODE_LOG_FILE) != 0;
}

std::string ToHex(const std::uint8_t *data, std::size_t length)
{
	std::string text;
	char byte[4];
	for (std::size_t index = 0; index < length; index++) {
		sprintf_s(byte, sizeof(byte), index ? " %02X" : "%02X", data[index]);
		text += byte;
	}
	return text;
}

std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point start,
	std::chrono::steady_clock::time_point end) noexcept
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

const char* ShareModeName(card_command::ShareMode mode) noexcept
{
	switch (mode) {
	case card_command::ShareMode::SHARED: return "shared";
	case card_command::ShareMode::EXCLUSIVE: return "exclusive";
	default: return "undefined";
	}
}

} // namespace

CardServer::CardServer(CardReaderDirectory &card_readers,
			       ReceiverManager &receiver_manager) noexcept
	: ServerBase(px4::ipc::Card, receiver_manager),
	card_readers_(card_readers),
	cards_mutex_()
{
	pipe_config_.in_buffer_size = sizeof(card_command::Command);
	pipe_config_.out_buffer_size = sizeof(card_command::Command);
	pipe_config_.stream_pipe = false;
	pipe_config_.stream_read = false;
	pipe_config_.default_timeout = 5000;
}

ServerBase::Connection* CardServer::CreateConnection(std::unique_ptr<PipeServer> &pipe)
{
	return new CardConnection(*this, pipe);
}

std::shared_ptr<CardServer::SharedCard> CardServer::GetCard(
	const std::wstring &reader_name, int &error)
{
	std::lock_guard<std::mutex> lock(cards_mutex_);
	auto device = card_readers_.FindCardReader(reader_name);
	if (!device) {
		error = -ENOENT;
		return nullptr;
	}

	auto existing = cards_.find(reader_name);
	if (existing != cards_.end()) {
		auto existing_card = existing->second.lock();
		/* 同名デバイスの再接続時は取り外し前の USB ハンドルを再利用しない */
		if (existing_card && existing_card->device == device) {
			error = 0;
			return existing_card;
		}
		cards_.erase(existing);
	}

	auto card = std::make_shared<SharedCard>(std::move(device));
	error = card->card.Open();
	if (error)
		return nullptr;

	cards_.emplace(reader_name, card);
	return card;
}

std::shared_ptr<CardServer::SharedCard> CardServer::AcquireCard(
	const std::wstring &reader_name, card_command::ShareMode share_mode,
	int &error)
{
	auto card = GetCard(reader_name, error);
	if (!card)
		return nullptr;

	std::lock_guard<std::mutex> lock(cards_mutex_);
	if (share_mode == card_command::ShareMode::EXCLUSIVE) {
		if (card->exclusive_connection || card->shared_connections) {
			error = -EBUSY;
			return nullptr;
		}
		card->exclusive_connection = true;
	} else if (share_mode == card_command::ShareMode::SHARED) {
		if (card->exclusive_connection) {
			error = -EBUSY;
			return nullptr;
		}
		card->shared_connections++;
	} else {
		error = -EINVAL;
		return nullptr;
	}

	error = 0;
	return card;
}

void CardServer::ReleaseCard(const std::shared_ptr<SharedCard> &card,
	card_command::ShareMode share_mode) noexcept
{
	std::lock_guard<std::mutex> lock(cards_mutex_);
	if (share_mode == card_command::ShareMode::EXCLUSIVE)
		card->exclusive_connection = false;
	else if (share_mode == card_command::ShareMode::SHARED && card->shared_connections)
		card->shared_connections--;
}

int CardServer::ChangeShareMode(const std::shared_ptr<SharedCard> &card,
	card_command::ShareMode current_mode,
	card_command::ShareMode requested_mode) noexcept
{
	if (current_mode == requested_mode)
		return 0;
	if (requested_mode != card_command::ShareMode::SHARED &&
		requested_mode != card_command::ShareMode::EXCLUSIVE)
		return -EINVAL;

	std::lock_guard<std::mutex> lock(cards_mutex_);
	if (requested_mode == card_command::ShareMode::EXCLUSIVE) {
		/* 自分自身以外の共有接続が残っている間は排他へ変更できない */
		if (card->exclusive_connection || card->shared_connections != 1)
			return -EBUSY;
		card->shared_connections = 0;
		card->exclusive_connection = true;
	} else {
		if (!card->exclusive_connection)
			return -EINVAL;
		card->exclusive_connection = false;
		card->shared_connections = 1;
	}
	return 0;
}

CardServer::CardConnection::CardConnection(CardServer &parent,
					   std::unique_ptr<PipeServer> &pipe) noexcept
	: Connection(parent, pipe),
	card_parent_(parent),
	share_mode_(card_command::ShareMode::UNDEFINED),
	transaction_(false)
{
}

void CardServer::CardConnection::ReleaseCard() noexcept
{
	if (!card_)
		return;
	LogTransmitSummary("release");
	if (CardLogEnabled())
		msg_info("release reader=%ls share=%s\n", reader_.c_str(), ShareModeName(share_mode_));
	stats_ = TransmitStats();
	if (transaction_) {
		card_->transaction_active = false;
		card_->io_mutex.unlock();
		transaction_ = false;
	}
	card_parent_.ReleaseCard(card_, share_mode_);
	card_.reset();
	share_mode_ = card_command::ShareMode::UNDEFINED;
	generation_ = 0;
	reader_.clear();
}

void CardServer::CardConnection::SetError(card_command::Command &command,
					  int error) const noexcept
{
	command.status = card_command::Status::FAILED;

	switch (error) {
	case -EINVAL:
	case -ENAMETOOLONG:
		command.error = card_command::Error::INVALID_PARAMETER;
		break;
	case -EBUSY:
	case -EALREADY:
		command.error = card_command::Error::BUSY;
		break;
	case SMART_CARD_NO_MEDIUM:
		command.error = card_command::Error::NO_CARD;
		break;
	case -ENODEV:
	case -ENXIO:
		command.error = card_command::Error::REMOVED;
		break;
	case -ETIMEDOUT:
		command.error = card_command::Error::TIMEOUT;
		break;
	case -ENOBUFS:
	case -EMSGSIZE:
		command.error = card_command::Error::INSUFFICIENT_BUFFER;
		break;
	case -EPROTO:
	case -EBADMSG:
		command.error = card_command::Error::PROTOCOL;
		break;
	default:
		command.error = card_command::Error::INTERNAL;
		break;
	}
}

void CardServer::CardConnection::LogTransmit(int ret, const std::uint8_t header[4],
	std::size_t send_length, const card_command::Command &command,
	std::size_t recv_length, std::uint64_t io_us, std::uint64_t wait_us,
	const SmartCard::Diagnostics &before,
	const SmartCard::Diagnostics &after) noexcept try
{
	if (!CardLogEnabled())
		return;

	const auto now = std::chrono::steady_clock::now();
	if (!stats_.has_baseline) {
		stats_.baseline = before;
		stats_.has_baseline = true;
		stats_.window_start = now;
	}
	stats_.latest = after;
	stats_.count++;
	stats_.total_us += io_us;
	stats_.max_us = (std::max)(stats_.max_us, io_us);
	stats_.max_wait_us = (std::max)(stats_.max_wait_us, wait_us);
	if (ret)
		stats_.failures++;

	const bool recovered = after.block_retries != before.block_retries ||
		after.wtx_requests != before.wtx_requests ||
		after.i_block_resends != before.i_block_resends ||
		after.response_recoveries != before.response_recoveries ||
		after.resets != before.resets;
	const bool slow = io_us >= SLOW_TRANSMIT_US;
	if (slow)
		stats_.slow++;

	/* 失敗、遅延、再送を伴った APDU だけを個別に記録し、時刻を TS のドロップと照合できるようにする */
	if (ret || slow || recovered) {
		unsigned int sw = 0;
		if (!ret && recv_length >= 2)
			sw = (static_cast<unsigned int>(command.data[recv_length - 2]) << 8) |
				command.data[recv_length - 1];
		/* APDU の本文には鍵を含む応答があるため、ヘッダー4バイトと長さ、状態語だけを残す */
		msg_printf("%s " msg_prefix ": transmit reader=%ls result=%d apdu=%02X%02X%02X%02X send=%zu recv=%zu sw=%04X io_ms=%.1f wait_ms=%.1f block_retry=+%llu wtx=+%llu i_resend=+%llu recover=+%llu reset=+%llu/%llu\n",
			ret ? "[WARN]" : "[INFO]",
			reader_.c_str(), ret, header[0], header[1], header[2], header[3],
			send_length, recv_length, sw, io_us / 1000.0, wait_us / 1000.0,
			static_cast<unsigned long long>(after.block_retries - before.block_retries),
			static_cast<unsigned long long>(after.wtx_requests - before.wtx_requests),
			static_cast<unsigned long long>(after.i_block_resends - before.i_block_resends),
			static_cast<unsigned long long>(after.response_recoveries - before.response_recoveries),
			static_cast<unsigned long long>(after.resets - before.resets),
			static_cast<unsigned long long>(after.reset_failures - before.reset_failures));
	}

	if (now - stats_.window_start >= SUMMARY_INTERVAL) {
		LogTransmitSummary("periodic");
		stats_.count = stats_.failures = stats_.slow = 0;
		stats_.total_us = stats_.max_us = stats_.max_wait_us = 0;
		stats_.baseline = after;
		stats_.window_start = now;
	}
}
catch (...) {
}

void CardServer::CardConnection::LogTransmitSummary(const char *reason) noexcept
{
	if (!CardLogEnabled() || !stats_.has_baseline)
		return;
	/*
	 * 累積値は最後の APDU の直後にカード I/O の排他下で写した値を使う
	 * 切断時に排他を取り直すと、他接続のトランザクションの終了まで切断が待たされる
	 */
	const auto &current = stats_.latest;
	const auto &base = stats_.baseline;
	const double seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - stats_.window_start).count();
	msg_info("summary reader=%ls reason=%s window_s=%.0f transmits=%llu failures=%llu slow=%llu avg_ms=%.1f max_ms=%.1f max_wait_ms=%.1f block_retry=%llu wtx=%llu i_resend=%llu recover=%llu reset=%llu reset_fail=%llu\n",
		reader_.c_str(), reason, seconds,
		static_cast<unsigned long long>(stats_.count),
		static_cast<unsigned long long>(stats_.failures),
		static_cast<unsigned long long>(stats_.slow),
		stats_.count ? stats_.total_us / 1000.0 / stats_.count : 0.0,
		stats_.max_us / 1000.0, stats_.max_wait_us / 1000.0,
		static_cast<unsigned long long>(current.block_retries - base.block_retries),
		static_cast<unsigned long long>(current.wtx_requests - base.wtx_requests),
		static_cast<unsigned long long>(current.i_block_resends - base.i_block_resends),
		static_cast<unsigned long long>(current.response_recoveries - base.response_recoveries),
		static_cast<unsigned long long>(current.resets - base.resets),
		static_cast<unsigned long long>(current.reset_failures - base.reset_failures));
}

void CardServer::CardConnection::LogStatus(const std::wstring &reader, int ret,
	const card_command::Command &command) noexcept try
{
	if (!CardLogEnabled())
		return;
	char text[160];
	sprintf_s(text, sizeof(text), "result=%d present=%d initialized=%d atr=",
		ret, ret ? 0 : command.card_present, ret ? 0 : command.card_initialized);
	std::string signature = text;
	if (!ret)
		signature += ToHex(command.atr,
			(std::min<std::size_t>)(command.atr_length, card_command::MAX_ATR_SIZE));
	auto &last = last_status_[reader];
	if (last == signature)
		return;
	last = signature;
	msg_info("status reader=%ls %s\n", reader.c_str(), signature.c_str());
}
catch (...) {
}

void CardServer::CardConnection::Worker() noexcept
{
	card_command::Command command = {};

	while (true) {
		std::size_t read = 0;
		if (!conn_->Read(&command, sizeof(command), read, quit_event_))
			break;
		if (read != sizeof(command))
			break;

		command.status = card_command::Status::SUCCEEDED;
		command.error = card_command::Error::NONE;

		try {
			switch (command.code) {
			case card_command::Code::GET_VERSION:
				command.version = card_command::VERSION;
				break;

			case card_command::Code::LIST_READERS:
			{
				auto readers = card_parent_.card_readers_.ListCardReaders();
				/* 固定長 IPC へコピーする前に全件を検査し、切り詰めた別名を公開しない */
				bool has_invalid_name = std::any_of(readers.begin(), readers.end(),
					[](const std::wstring &reader) {
						return reader.size() >= card_command::MAX_READER_NAME;
					});
				if (has_invalid_name) {
					SetError(command, -ENAMETOOLONG);
					break;
				}
				command.reader_count = static_cast<std::uint32_t>(
					std::min<std::size_t>(readers.size(), card_command::MAX_READERS));
				command.reader_generation =
					card_parent_.card_readers_.GetCardReaderGeneration();
				for (std::size_t index = 0; index < command.reader_count; index++)
					wcscpy_s(command.readers[index], readers[index].c_str());
				break;
			}

			case card_command::Code::CONNECT:
			{
				if (!wmemchr(command.reader_name, L'\0',
					card_command::MAX_READER_NAME)) {
					SetError(command, -EINVAL);
					break;
				}
				int ret = 0;
				auto share_mode = static_cast<card_command::ShareMode>(command.share_mode);
				if (share_mode != card_command::ShareMode::SHARED &&
					share_mode != card_command::ShareMode::EXCLUSIVE) {
					SetError(command, -EINVAL);
					break;
				}
				ReleaseCard();
				auto card = card_parent_.AcquireCard(command.reader_name, share_mode, ret);
				if (!card) {
					command.status = card_command::Status::FAILED;
					if (ret == -ENOENT)
						command.error = card_command::Error::NO_READER;
					else
						SetError(command, ret);
					if (CardLogEnabled())
						msg_warn("connect reader=%ls share=%s result=%d\n",
							command.reader_name, ShareModeName(share_mode), ret);
					break;
				}
				card_ = std::move(card);
				share_mode_ = share_mode;
				reader_ = command.reader_name;
				{
					std::lock_guard<std::recursive_mutex> lock(card_->io_mutex);
					ret = card_->card.BindConnection(generation_);
				}
				if (CardLogEnabled())
					msg_printf("%s " msg_prefix ": connect reader=%ls share=%s result=%d\n",
						ret ? "[WARN]" : "[INFO]", reader_.c_str(),
						ShareModeName(share_mode), ret);
				if (ret) { SetError(command, ret); ReleaseCard(); }
				break;
			}

			case card_command::Code::RECONNECT:
			{
				if (!card_) {
					SetError(command, -ENODEV);
					break;
				}
				auto requested_mode =
					static_cast<card_command::ShareMode>(command.share_mode);
				int ret = 0;
				/*
				 * 接続が持つ SharedCard は接続した時点の DeviceBase に結び付いている
				 * 同じリーダー名の機器を挿し直すと別の DeviceBase が作られるため、古い機器へ再接続を続けると
				 * SCardReconnect() で回復する利用側が復旧できない。現在の機器が別なら、CONNECT と同じく取得し直す
				 * 機器が戻っていない間は従来どおり古い機器で失敗させ、抜去 (REMOVED) を返す
				 */
				auto current_device = card_parent_.card_readers_.FindCardReader(reader_);
				if (current_device && current_device != card_->device) {
					const std::wstring reader = reader_;
					ReleaseCard();
					auto card = card_parent_.AcquireCard(reader, requested_mode, ret);
					if (!card) {
						command.status = card_command::Status::FAILED;
						if (ret == -ENOENT)
							command.error = card_command::Error::NO_READER;
						else
							SetError(command, ret);
						if (CardLogEnabled())
							msg_warn("reconnect reader=%ls share=%s result=%d (replugged device)\n",
								reader.c_str(), ShareModeName(requested_mode), ret);
						break;
					}
					card_ = std::move(card);
					share_mode_ = requested_mode;
					reader_ = reader;
				} else {
					ret = card_parent_.ChangeShareMode(card_, share_mode_, requested_mode);
				}
				if (!ret) {
					share_mode_ = requested_mode;
					std::lock_guard<std::recursive_mutex> lock(card_->io_mutex);
					ret = card_->card.BindConnection(generation_);
				}
				if (CardLogEnabled())
					msg_printf("%s " msg_prefix ": reconnect reader=%ls share=%s result=%d\n",
						ret ? "[WARN]" : "[INFO]", reader_.c_str(),
						ShareModeName(requested_mode), ret);
				if (ret) SetError(command, ret);
				break;
			}

			case card_command::Code::DISCONNECT:
				ReleaseCard();
				break;

			case card_command::Code::STATUS:
			{
				if (!wmemchr(command.reader_name, L'\0',
					card_command::MAX_READER_NAME)) {
					SetError(command, -EINVAL);
					break;
				}
				std::vector<std::uint8_t> atr;
				int ret;
				std::shared_ptr<SharedCard> status_card;
				if (card_ && reader_ == command.reader_name)
					status_card = card_;
				if (!status_card) {
					auto monitored = monitored_cards_.find(command.reader_name);
					if (monitored != monitored_cards_.end()) {
						/* USB 再接続後は同じ名前でも新しいデバイスを監視する */
						auto current_device = card_parent_.card_readers_.FindCardReader(
							command.reader_name);
						if (current_device && monitored->second->device == current_device)
							status_card = monitored->second;
						else
							monitored_cards_.erase(monitored);
					}
				}
				if (!status_card) {
					status_card = card_parent_.GetCard(command.reader_name, ret);
					if (!status_card) {
						command.status = card_command::Status::FAILED;
						if (ret == -ENOENT)
							command.error = card_command::Error::NO_READER;
						else
							SetError(command, ret);
						LogStatus(command.reader_name, ret, command);
						break;
					}
					/* 監視用パイプが生きている間だけカード状態と ATR を保持する */
					monitored_cards_.emplace(command.reader_name, status_card);
				}
				/* 状態監視を他接続のトランザクション待ちへ入れると、API 側の期限とキャンセルを確認できない */
				std::unique_lock<std::recursive_mutex> lock(status_card->io_mutex, std::defer_lock);
				if (!LockCardForStatus(lock, status_card->transaction_active)) {
					SetError(command, -EBUSY);
					break;
				}
				ret = status_card->card.GetStatus(command.card_present,
					command.card_initialized, atr);
				/* 在否の照会は GetStatus の1回だけにし、その観測で自接続の失効を判定する */
				if (!ret && status_card == card_)
					ret = card_->card.CheckGeneration(generation_);
				lock.unlock();
				if (ret) {
					SetError(command, ret);
					LogStatus(command.reader_name, ret, command);
					break;
				}
				command.atr_length = static_cast<std::uint32_t>(atr.size());
				if (!atr.empty())
					memcpy(command.atr, atr.data(), atr.size());
				/* 他接続のトランザクション中の -EBUSY は短い周期で交互に現れるため、記録の対象から外す */
				LogStatus(command.reader_name, 0, command);
				break;
			}

			case card_command::Code::RESET:
			{
				if (!card_) {
					SetError(command, -ENODEV);
					break;
				}
				std::vector<std::uint8_t> atr;
				int ret;
				SmartCard::Diagnostics diagnostics;
				std::uint64_t io_us;
				{
					std::lock_guard<std::recursive_mutex> lock(card_->io_mutex);
					const auto start = std::chrono::steady_clock::now();
					ret = card_->card.ResetConnection(generation_, atr);
					io_us = ElapsedUs(start, std::chrono::steady_clock::now());
					diagnostics = card_->card.GetDiagnostics();
				}
				if (CardLogEnabled())
					msg_printf("%s " msg_prefix ": reset reader=%ls result=%d io_ms=%.1f atr=%s baud=%u ifsc=%u crc=%d bwt_ms=%u atr_retry_total=%llu\n",
						ret ? "[WARN]" : "[INFO]", reader_.c_str(), ret, io_us / 1000.0,
						ToHex(atr.data(), atr.size()).c_str(),
						ret ? 0U : diagnostics.baudrate,
						ret ? 0U : static_cast<unsigned int>(diagnostics.ifsc),
						ret ? 0 : static_cast<int>(diagnostics.use_crc),
						ret ? 0U : diagnostics.block_timeout_ms,
						static_cast<unsigned long long>(diagnostics.atr_retries));
				if (ret) {
					SetError(command, ret);
					break;
				}
				command.atr_length = static_cast<std::uint32_t>(atr.size());
				if (!atr.empty())
					memcpy(command.atr, atr.data(), atr.size());
				command.card_present = true;
				command.card_initialized = true;
				break;
			}

			case card_command::Code::TRANSMIT:
			{
				if (!card_ || command.data_length > card_command::MAX_DATA_SIZE ||
					command.data_capacity > card_command::MAX_DATA_SIZE) {
					SetError(command, -EINVAL);
					break;
				}
				/* 必要サイズを返せるよう、共有コマンド領域の全容量で受信する */
				std::size_t recv_length = card_command::MAX_DATA_SIZE;
				/* 応答で上書きされる前に、診断ログ用の APDU ヘッダーを写す */
				std::uint8_t header[4] = {};
				const std::size_t send_length = command.data_length;
				memcpy(header, command.data, (std::min<std::size_t>)(send_length, sizeof(header)));
				SmartCard::Diagnostics before, after;
				std::uint64_t io_us, wait_us;
				int ret;
				const auto wait_start = std::chrono::steady_clock::now();
				{
					std::lock_guard<std::recursive_mutex> lock(card_->io_mutex);
					const auto io_start = std::chrono::steady_clock::now();
					before = card_->card.GetDiagnostics();
					ret = card_->card.TransmitConnection(generation_, command.data,
						command.data_length, command.data, recv_length);
					after = card_->card.GetDiagnostics();
					io_us = ElapsedUs(io_start, std::chrono::steady_clock::now());
					wait_us = ElapsedUs(wait_start, io_start);
				}
				command.data_length = static_cast<std::uint32_t>(recv_length);
				if (!ret && recv_length > command.data_capacity)
					ret = -ENOBUFS;
				if (ret)
					SetError(command, ret);
				/* ログの書き込みはカード I/O の排他を解放してから行い、他接続の APDU を待たせない */
				LogTransmit(ret, header, send_length, command, recv_length, io_us, wait_us,
					before, after);
				break;
			}

			case card_command::Code::BEGIN_TRANSACTION:
				if (!card_ || transaction_) {
					SetError(command, -EINVAL);
					break;
				}
				card_->io_mutex.lock();
				{
					int ret = card_->card.CheckConnection(generation_);
					if (ret) { card_->io_mutex.unlock(); SetError(command, ret); break; }
				}
				card_->transaction_active = true;
				transaction_ = true;
				break;

			case card_command::Code::END_TRANSACTION:
				if (!card_ || !transaction_) {
					SetError(command, -EINVAL);
					break;
				}
				{
					auto disposition = static_cast<card_command::Disposition>(
						command.disposition);
					int ret = 0;
					if (disposition != card_command::Disposition::LEAVE &&
						disposition != card_command::Disposition::RESET) {
						ret = -EINVAL;
					} else if (disposition == card_command::Disposition::RESET) {
						try {
							std::vector<std::uint8_t> atr;
							ret = card_->card.ResetConnection(generation_, atr);
							if (!ret) {
								command.atr_length = static_cast<std::uint32_t>(atr.size());
								if (!atr.empty())
									memcpy(command.atr, atr.data(), atr.size());
							}
						} catch (...) {
							ret = -EIO;
						}
					}
					/* disposition の成否にかかわらず、終了要求で排他状態を解除する */
					card_->transaction_active = false;
					card_->io_mutex.unlock();
					transaction_ = false;
					if (ret)
						SetError(command, ret);
				}
				break;

			default:
				SetError(command, -EINVAL);
				break;
			}
		} catch (...) {
			SetError(command, -EIO);
			if (CardLogEnabled())
				msg_err("exception code=%u reader=%ls\n",
					static_cast<unsigned int>(command.code), reader_.c_str());
		}

		if (CardLogEnabled() && command.status == card_command::Status::FAILED &&
			(command.code == card_command::Code::BEGIN_TRANSACTION ||
			 command.code == card_command::Code::END_TRANSACTION))
			msg_warn("transaction code=%u reader=%ls error=%u disposition=%u\n",
				static_cast<unsigned int>(command.code), reader_.c_str(),
				static_cast<unsigned int>(command.error), command.disposition);

		std::size_t written = 0;
		if (!conn_->Write(&command, sizeof(command), written, quit_event_) || written != sizeof(command))
			break;
	}

	ReleaseCard();
	delete this;
}

} // namespace px4
