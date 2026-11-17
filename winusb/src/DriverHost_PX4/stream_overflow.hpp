// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include "msg.h"

namespace px4 {

// 受信バッファの満杯で書き込めなかった TS を捨て、受信を続けるときの記録。
// 利用者の読み出しが一時的に遅れても受信を止めると、選局し直すまで録画が失われるため、
// PX4 系と同じく入りきらない分だけを捨てる。捨てる位置は、188バイト単位の書き込みと読み出しで
// パケット境界に揃う。満杯になった時点と解消した時点だけを記録し、長い満杯でログを増やし続けない。
// 1つの書き込みスレッドから呼ぶ。
class StreamOverflowLog final {
public:
	explicit StreamOverflowLog(const char *name) noexcept : name_(name) {}

	// StreamBuffer::Write の戻り値と、要求した長さ、書き込めた長さを渡す。
	void Record(bool written, std::size_t requested, std::size_t accepted) noexcept
	{
		// true で短い長さは、利用者の Purge による意図した破棄で、満杯ではない。
		if (written) {
			if (full_ && accepted == requested) {
				msg_warn("%s: TS buffer accepts data again dropped_bytes=%llu\n", name_,
					static_cast<unsigned long long>(dropped_));
				full_ = false;
				dropped_ = 0;
			}
			return;
		}
		if (!full_)
			msg_warn("%s: TS buffer full, dropping TS until the reader catches up\n", name_);
		full_ = true;
		dropped_ += requested - accepted;
	}

private:
	const char *name_;
	bool full_ = false;
	std::uint64_t dropped_ = 0;
};

}
