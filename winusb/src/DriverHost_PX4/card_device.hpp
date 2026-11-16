#pragma once

#include <cstddef>
#include <cstdint>
#include <cerrno>

namespace px4 {

class CardDevice {
public:
	virtual ~CardDevice() = default;

	virtual int OpenCard() = 0;
	virtual void CloseCard() = 0;
	virtual int DetectCard(bool &detected) = 0;
	virtual int ResetCard() = 0;
	/* baudrate は bps で渡す (9600 、19200 、38400 のいずれか) */
	virtual int SetCardBaudrate(std::uint32_t baudrate) = 0;
	/* ready は UART に完全なカード応答が揃った場合だけ true を返す */
	virtual int IsCardDataReady(bool &ready) = 0;
	/*
	 * length は UART に溜まった受信バイト数を返す。受信途中でも増えるため完成判定には使えない。
	 * IT930x はスカパー！プレミアムサービスのカードの ATR (10バイト) を受信し終えても
	 * RX_READY を立てないため、ATR だけは長さの変化が止まったことを見て読み出す。
	 * ReadCardData() が溜まった分をそのまま返す機種だけが実装し、それ以外は -EOPNOTSUPP を返す。
	 */
	virtual int GetCardDataLength(std::size_t &length) { (void)length; return -EOPNOTSUPP; }
	/* len は呼び出し前に buf の容量、成功後に実際の読み取り長を表す */
	virtual int ReadCardData(std::uint8_t *buf, std::uint8_t &len) = 0;
	/* len は buf から送信するバイト数を表す */
	virtual int WriteCardData(const std::uint8_t *buf, std::uint8_t len) = 0;
};

} // namespace px4
