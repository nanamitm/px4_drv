#pragma once

/*
 * smart_card_state_test と card_server_reconnect_test が共有する B-CAS カードの模擬
 * ATR、T=1 の S ブロックと I ブロックの応答、送受信の失敗を再現する
 */
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <new>
#include <vector>

#include "card_device.hpp"
#include "../src/DriverHost_PX4/dt295/card_response.hpp"

namespace {

class MockCardDevice final : public px4::CardDevice {
public:
	int OpenCard() override
	{
		is_open = true;
		return 0;
	}

	void CloseCard() override
	{
		is_open = false;
	}

	int DetectCard(bool &detected) override
	{
		detect_count++;
		if (detect_error) {
			int error = detect_error;
			detect_error = 0;
			return error;
		}
		detected = is_present;
		return 0;
	}

	int ResetCard() override
	{
		if (!is_present)
			return -ENODEV;
		reset_count++;
		dt295_response.Reset();
		read_queue.clear();
		/* セッション再構築でカード側の N(S) も初期値へ戻る */
		response_sequence = 0;
		/* 前のセッションの応答を、再送要求で送り直してしまわないよう捨てる */
		last_response_pcb = 0;
		last_response.clear();
		final_response_lost = false;
		if (malformed_atr_resets) {
			malformed_atr_resets--;
			read_queue.insert(read_queue.end(), malformed_atr_bytes.begin(),
				malformed_atr_bytes.end());
		} else {
			read_queue.insert(read_queue.end(), atr_bytes.begin(), atr_bytes.end());
		}
		atr_pending_without_ready = atr_without_ready;
		return 0;
	}

	int SetCardBaudrate(std::uint32_t value) override
	{
		baudrate = value;
		return 0;
	}

	int IsCardDataReady(bool &ready) override
	{
		if (use_dt295_response) {
			// UARTから1バイトずつ届く場合も、製品コードの完成判定を経由する。
			if (!dt295_response.Ready() && !read_queue.empty()) {
				const auto byte = read_queue.front();
				read_queue.pop_front();
				if (const int rc = dt295_response.Append(&byte, 1)) return rc;
			}
			ready = dt295_response.Ready();
			return 0;
		}
		if (ready_delay_checks) {
			ready_delay_checks--;
			ready = false;
			read_allowed = false;
			return 0;
		}
		/* IT930x が一部カードの ATR で RX_READY を立てない状態を再現する */
		if (atr_pending_without_ready) {
			ready = false;
			read_allowed = false;
			return 0;
		}
		ready = !read_queue.empty();
		read_allowed = ready;
		return 0;
	}

	int GetCardDataLength(std::size_t &length) override
	{
		if (!report_length)
			return -EOPNOTSUPP;
		length_queried = true;
		length = read_queue.size();
		return 0;
	}

	int ReadCardData(std::uint8_t *buffer, std::uint8_t &length) override
	{
		if (use_dt295_response) {
			length = static_cast<std::uint8_t>(dt295_response.Read(buffer, length));
			return 0;
		}
		if (atr_pending_without_ready) {
			atr_read_without_ready = true;
		} else if (!read_allowed) {
			read_called_before_ready = true;
		}
		read_allowed = false;
		std::size_t copy_length = std::min<std::size_t>(length, read_queue.size());
		for (std::size_t index = 0; index < copy_length; index++) {
			buffer[index] = read_queue.front();
			read_queue.pop_front();
		}
		length = static_cast<std::uint8_t>(copy_length);
		if (read_queue.empty())
			atr_pending_without_ready = false;
		return 0;
	}

	int WriteCardData(const std::uint8_t *buffer, std::uint8_t length) override
	{
		if (write_error) { int error = write_error; write_error = 0; return error; }
		if (throw_next_apdu) {
			throw_next_apdu = false;
			throw std::bad_alloc();
		}
		if (length < 4)
			return -EINVAL;
		const std::uint8_t pcb = buffer[1];
		written_pcbs.push_back(pcb);
		const std::uint8_t data_length = buffer[2];
		if (pcb == 0xc1 && data_length == 1)
			ifs_values.push_back(buffer[3]);
		std::size_t expected_length = static_cast<std::size_t>(data_length) +
			(expected_crc ? 5 : 4);
		if (expected_length != length || !ValidateEdc(buffer, length))
			return -EINVAL;
		if (drop_first_ifs && pcb == 0xc1) {
			drop_first_ifs = false;
			return 0;
		}

		/* スカパー！プレミアムサービスのカードは RESYNCH へ状態語 6E00 を付けた C0 を返す */
		if (resynch_unsupported && pcb == 0xc0) {
			const std::uint8_t status_word[] = { 0x6e, 0x00 };
			QueueBlock(0xc0, status_word, sizeof(status_word));
			return 0;
		}
		if (resynch_empty_reply && pcb == 0xc0) {
			QueueBlock(0xc0, nullptr, 0);
			return 0;
		}
		/* 初期化 S ブロックは要求データをそのまま応答する */
		if ((pcb & 0xc0) == 0xc0) {
			if (endless_wtx && pcb == 0xe3) {
				const std::uint8_t multiplier = 1;
				QueueBlock(0xc3, &multiplier, 1);
				return 0;
			}
			QueueBlock(static_cast<std::uint8_t>(pcb | 0x20),
				buffer + 3, data_length);
			return 0;
		}

		if (fail_next_apdu) {
			fail_next_apdu = false;
			return -EIO;
		}
		if (endless_wtx) {
			const std::uint8_t multiplier = 1;
			QueueBlock(0xc3, &multiplier, 1);
			return 0;
		}
		/* 連結 I ブロックを受理したら次の送信連番を R ブロックで要求する */
		if ((pcb & 0x80) == 0) {
			apdu_block_lengths.push_back(data_length);
			if (pcb & 0x20) {
				const std::uint8_t next_sequence = (pcb & 0x40) ? 0 : 0x10;
				QueueBlock(static_cast<std::uint8_t>(0x80 | next_sequence),
					buffer, 0);
				return 0;
			}
		}
		/*
		 * 応答を取りこぼしたホストは R ブロックで再送を要求する
		 * 要求された N(R) が保留中の応答の N(S) と一致するときだけ、連番を進めず
		 * 直前と同じ I ブロックを送り直す
		 * 一致しない要求は再送とみなさず、連番の食い違いをホスト側に検出させる
		 */
		if ((pcb & 0xc0) == 0x80 && !last_response.empty()) {
			const std::uint8_t expected_sequence =
				(last_response_pcb & 0x40) ? 0x10 : 0x00;

			if ((pcb & 0x10) == expected_sequence) {
				cached_resend_count++;
				QueueBlock(last_response_pcb, last_response.data(),
					last_response.size());
				return 0;
			}
		}
		/*
		 * 応答を落とした後に同じ I ブロックが再送されてきた状態
		 * カードは受理済みなので連番を進めず、受理を示す R ブロックだけを返す
		 */
		if (final_response_lost) {
			final_response_lost = false;
			const std::uint8_t next_sequence = (pcb & 0x40) ? 0 : 0x10;
			QueueBlock(static_cast<std::uint8_t>(0x80 | next_sequence),
				buffer, 0);
			return 0;
		}
		/* I ブロック応答はカード側の N(S) を交互に進める */
		const std::uint8_t response_pcb = response_sequence ? 0x40 : 0x00;
		response_sequence ^= 1;
		if (lose_final_response) {
			lose_final_response = false;
			final_response_lost = true;
			/*
			 * カードは I ブロックを受理して応答を作ったが、それが届かなかった状態を作る
			 * 何も返さないため、ホストは待機時間を使い切って I ブロックを再送する
			 */
			last_response_pcb = response_pcb;
			last_response = { 0x90, 0x00 };
			return 0;
		}
		if (large_duplicate_response) {
			large_duplicate_response = false;
			/* 2フレームの合計が1回の読み出し上限255バイトを超える長さにする */
			const std::vector<std::uint8_t> large(200, 0xa5);
			QueueBlock(response_pcb, large.data(), large.size());
			QueueBlock(response_pcb, large.data(), large.size());
			return 0;
		}
		if (max_duplicate_response) {
			max_duplicate_response = false;
			/* 1フレームが読み出し上限とちょうど同じ255バイトになる長さにする */
			const std::vector<std::uint8_t> max_inf(251, 0x5a);
			QueueBlock(response_pcb, max_inf.data(), max_inf.size());
			QueueBlock(response_pcb, max_inf.data(), max_inf.size());
			return 0;
		}
		if (zero_frame_response) {
			zero_frame_response = false;
			/* 抜去の途中に UART が読んだ 00 だけの4バイト (空の I ブロックと同じ形) を返す */
			QueueBytes({ 0x00, 0x00, 0x00, 0x00 });
			return 0;
		}
		const std::uint8_t response[] = { 0x90, 0x00 };
		last_response_pcb = response_pcb;
		last_response.assign(response, response + sizeof(response));
		QueueBlock(response_pcb, response, sizeof(response));
		if (duplicate_next_response) {
			duplicate_next_response = false;
			QueueBlock(response_pcb, response, sizeof(response));
		}
		if (delay_next_response) {
			delay_next_response = false;
			ready_delay_checks = 3;
		}
		return 0;
	}

	void QueueBytes(std::initializer_list<std::uint8_t> bytes)
	{
		read_queue.insert(read_queue.end(), bytes.begin(), bytes.end());
	}

	void QueueBlock(std::uint8_t pcb, const std::uint8_t *data,
		std::size_t data_length)
	{
		std::vector<std::uint8_t> frame = {
			0x00,
			pcb,
			static_cast<std::uint8_t>(data_length),
		};
		frame.insert(frame.end(), data, data + data_length);
		if (expected_crc) {
			std::uint16_t crc = CalculateCrc(frame.data(), frame.size());
			frame.push_back(static_cast<std::uint8_t>(crc >> 8));
			frame.push_back(static_cast<std::uint8_t>(crc & 0xff));
		} else {
			std::uint8_t lrc = 0;
			for (std::uint8_t value : frame)
				lrc ^= value;
			frame.push_back(lrc);
		}
		read_queue.insert(read_queue.end(), frame.begin(), frame.end());
	}

	bool ValidateEdc(const std::uint8_t *frame, std::size_t length) const
	{
		if (expected_crc) {
			std::uint16_t crc = CalculateCrc(frame, length - 2);
			return frame[length - 2] == static_cast<std::uint8_t>(crc >> 8) &&
				frame[length - 1] == static_cast<std::uint8_t>(crc & 0xff);
		}
		std::uint8_t lrc = 0;
		for (std::size_t index = 0; index < length; index++)
			lrc ^= frame[index];
		return lrc == 0;
	}

	static std::uint16_t CalculateCrc(const std::uint8_t *data, std::size_t length)
	{
		std::uint16_t crc = 0xffff;
		for (std::size_t index = 0; index < length; index++) {
			crc ^= static_cast<std::uint16_t>(data[index]) << 8;
			for (unsigned int bit = 0; bit < 8; bit++)
				crc = (crc & 0x8000) ?
					static_cast<std::uint16_t>((crc << 1) ^ 0x1021) :
					static_cast<std::uint16_t>(crc << 1);
		}
		return crc;
	}

	bool is_open = false;
	bool is_present = false;
	bool fail_next_apdu = false;
	bool throw_next_apdu = false;
	bool endless_wtx = false;
	bool delay_next_response = false;
	bool duplicate_next_response = false;
	bool large_duplicate_response = false;
	bool max_duplicate_response = false;
	bool lose_final_response = false;
	bool zero_frame_response = false;
	bool final_response_lost = false;
	unsigned int cached_resend_count = 0;
	std::uint8_t response_sequence = 0;
	std::uint8_t last_response_pcb = 0;
	std::vector<std::uint8_t> last_response;
	bool drop_first_ifs = false;
	bool read_called_before_ready = false;
	bool read_allowed = false;
	bool atr_without_ready = false;
	bool atr_pending_without_ready = false;
	bool atr_read_without_ready = false;
	bool report_length = false;
	bool length_queried = false;
	bool resynch_unsupported = false;
	bool resynch_empty_reply = false;
	unsigned int ready_delay_checks = 0;
	int detect_error = 0;
	int detect_count = 0;
	int write_error = 0;
	unsigned int reset_count = 0;
	unsigned int malformed_atr_resets = 0;
	std::vector<std::uint8_t> malformed_atr_bytes = { 0xfc, 0xff };
	std::vector<std::uint8_t> atr_bytes = {
		0x3b, 0xf0, 0x12, 0x00, 0xff, 0x91, 0x81,
		0xb1, 0x7c, 0x45, 0x1f, 0x01, 0x9b,
	};
	std::uint32_t baudrate = 9600;
	bool expected_crc = false;
	bool use_dt295_response = false;
	px4::dt295::CardResponse dt295_response;
	std::vector<std::uint8_t> written_pcbs;
	std::vector<std::uint8_t> ifs_values;
	std::vector<std::uint8_t> apdu_block_lengths;
	std::deque<std::uint8_t> read_queue;
};

} // namespace
