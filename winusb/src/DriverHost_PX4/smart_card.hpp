#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "card_device.hpp"

namespace px4 {

constexpr int SMART_CARD_NO_MEDIUM = -10001;

class SmartCard final {
public:
	/*
	 * 診断ログ用の累積回数。プロトコル処理はログ出力を持たず回数だけを数え、
	 * 呼び出し側が要求の前後の差分から再送や WTX の発生を記録する。
	 */
	struct Diagnostics final {
		std::uint64_t resets = 0;
		std::uint64_t reset_failures = 0;
		/* 形式不正の ATR を受けて ResetCard() からやり直した回数 */
		std::uint64_t atr_retries = 0;
		/* 応答の期限切れまたは検査値不一致で同じブロックを再送した回数 */
		std::uint64_t block_retries = 0;
		std::uint64_t wtx_requests = 0;
		/* カードが R ブロックで同じ I ブロックの再送を要求した回数 */
		std::uint64_t i_block_resends = 0;
		/* 取りこぼしたカードの I ブロックを R ブロックで再要求した回数 */
		std::uint64_t response_recoveries = 0;
		std::uint32_t baudrate = 0;
		std::uint8_t ifsc = 0;
		bool use_crc = false;
		unsigned int block_timeout_ms = 0;
	};

	explicit SmartCard(std::shared_ptr<CardDevice> device) noexcept;
	~SmartCard();

	SmartCard(const SmartCard &) = delete;
	SmartCard& operator=(const SmartCard &) = delete;
	SmartCard(SmartCard &&) = delete;
	SmartCard& operator=(SmartCard &&) = delete;

	int Open();
	void Close() noexcept;
	int GetStatus(bool &present, bool &initialized, std::vector<std::uint8_t> &atr);
	int Reset(std::vector<std::uint8_t> &atr);
	/* 呼び出し元がカード I/O の排他を保持する。世代検査はAPDUを送信しない。 */
	int BindConnection(std::uint64_t &generation);
	int CheckConnection(std::uint64_t generation);
	/* 在否を照会せず、最後に観測したカードの世代と比べる。 */
	int CheckGeneration(std::uint64_t generation) const noexcept;
	/*
	 * Reset/Transmit は内部で在否を照会するため、事前の照会を重ねず、
	 * その観測で抜去や USB 切断が分かった場合も接続の失効として返す。
	 */
	int ResetConnection(std::uint64_t generation, std::vector<std::uint8_t> &atr);
	int TransmitConnection(std::uint64_t generation, const std::uint8_t *send_buf,
		std::size_t send_len, std::uint8_t *recv_buf, std::size_t &recv_len);
	int Transmit(const std::uint8_t *send_buf, std::size_t send_len,
		     std::uint8_t *recv_buf, std::size_t &recv_len);
	/* 呼び出し元がカード I/O の排他を保持する。 */
	const Diagnostics& GetDiagnostics() const noexcept { return diagnostics_; }

private:
	using Deadline = std::chrono::steady_clock::time_point;

	struct AtrParameters final {
		std::uint32_t baudrate = 9600;
		std::uint8_t ifsc = 32;
		bool use_crc = false;
		unsigned int block_timeout_ms = 500;
	};

	int ReadAtr(std::vector<std::uint8_t> &atr, AtrParameters &parameters);
	int WaitCardDataReady(const Deadline &deadline);
	int DiscardPendingCardData();
	int ParseAtr(const std::vector<std::uint8_t> &atr,
		     std::size_t &expected_length, AtrParameters &parameters) const;
	int InitializeT1(bool resynchronize, std::uint8_t ifsd);
	int SendBlock(std::uint8_t pcb, const std::uint8_t *data, std::size_t length);
	int ReceiveBlock(std::uint8_t &pcb, std::vector<std::uint8_t> &data,
			 const Deadline &deadline);
	int ExchangeBlock(std::uint8_t pcb, const std::uint8_t *send_data,
			  std::size_t send_length, std::uint8_t &recv_pcb,
			  std::vector<std::uint8_t> &recv_data,
			  const Deadline &deadline, unsigned int max_retries = 3);
	int TransmitInitialized(const std::uint8_t *send_buf, std::size_t send_len,
				std::uint8_t *recv_buf, std::size_t &recv_len,
				const Deadline &deadline);
	std::uint16_t CalculateCrc(const std::uint8_t *data, std::size_t length) const;
	void InvalidateSession(int cause = 0) noexcept;
	int ProbePresence(bool &present);

	std::shared_ptr<CardDevice> device_;
	bool open_;
	bool present_;
	bool initialized_;
	bool use_crc_;
	std::uint8_t card_ifsc_;
	unsigned int block_timeout_ms_;
	std::uint8_t send_sequence_;
	std::uint8_t receive_sequence_;
	std::vector<std::uint8_t> atr_;
	std::uint64_t generation_ = 1;
	bool presence_known_ = false;
	bool observed_present_ = false;
	Diagnostics diagnostics_;
};

} // namespace px4
