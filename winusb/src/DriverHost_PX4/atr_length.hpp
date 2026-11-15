#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace px4 {

/*
 * ATR の先頭から、ATR 全体のバイト数を求める
 * カード UART の受信を1バイトずつ溜める機種 (PIX-DT295 、torne 、GV-MVP/XZ3) が、
 * ATR の受信を終えたかを判定するために使う
 * 既知の B-CAS の ATR との一致で判定すると、スカパー！プレミアムサービスのカードの
 * ATR (10バイト) のように形式の正しい別の ATR を受信し終えたことを判定できない
 * 値の妥当性 (TA1 の速度や TCK) は SmartCard::ParseAtr() が検査する
 *
 * @param atr 受信済みの ATR
 * @param length 受信済みのバイト数
 * @param expected_length 成功時に ATR 全体のバイト数
 * @returns 全体の長さが確定したら0、判定に必要なバイトが足りなければ -EAGAIN 、
 *          ATR の形式でなければ -EBADMSG
 */
inline int GetAtrLength(const std::uint8_t *atr, std::size_t length,
	std::size_t &expected_length) noexcept
{
	constexpr std::size_t MAX_ATR_LENGTH = 33;

	if (!length)
		return -EAGAIN;
	if (!atr || (atr[0] != 0x3b && atr[0] != 0x3f))
		return -EBADMSG;
	if (length < 2)
		return -EAGAIN;

	std::uint8_t interfaces = atr[1] >> 4;
	const std::size_t historical_length = atr[1] & 0x0f;
	std::size_t offset = 2;
	bool has_non_t0_protocol = false;

	while (interfaces) {
		/* TA 、TB 、TC の有無を数え、TD があれば次のグループへ進む */
		for (unsigned int bit = 0; bit < 3; bit++) {
			if (interfaces & (1U << bit))
				offset++;
		}
		if (!(interfaces & 0x08))
			break;
		if (offset >= length)
			return -EAGAIN;
		const std::uint8_t td = atr[offset++];
		has_non_t0_protocol = has_non_t0_protocol || (td & 0x0f) != 0;
		interfaces = td >> 4;
		if (offset > MAX_ATR_LENGTH)
			return -EBADMSG;
	}

	/* T=0 以外のプロトコルを示す ATR は末尾に TCK を持つ */
	expected_length = offset + historical_length + (has_non_t0_protocol ? 1 : 0);
	if (expected_length > MAX_ATR_LENGTH)
		return -EBADMSG;
	return 0;
}

} // namespace px4
