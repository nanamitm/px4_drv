// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace px4::xz3 {

// 操作12の送出量の段階。ファームウェアのSTREAM_UNITと一致させる。
constexpr unsigned PumpStages = 4;
constexpr unsigned PumpSize(unsigned stage) { return (3u * 1024 * 1024) >> stage; }
constexpr unsigned RingBytes = 0x600000;

inline bool ValidPumpReply(unsigned status, unsigned stage, unsigned sent,
                           unsigned cursor, unsigned producer)
{
    const auto target = PumpSize(stage);
    // status=2は送出不足。範囲外のDMA位置や512バイト境界を外れた応答は通信異常とする。
    if (cursor >= RingBytes || producer >= RingBytes || (cursor % 512) || (sent % 512))
        return false;
    return status == 0 ? sent == target : status == 2 && sent < target;
}

class PumpProgress {
public:
    unsigned Stage(unsigned preferred, std::size_t capacity) const
    {
        // 先頭の読み出しをまたいで送れるが、発行済みの2本の残り容量を超えない。
        const auto free = capacity - pending_;
        while (preferred + 1 < PumpStages && PumpSize(preferred) > free)
            ++preferred;
        return preferred;
    }

    void Sent(std::size_t bytes) { pending_ += bytes; }
    bool Complete(std::size_t size) const { return pending_ >= size; }
    void Collected(std::size_t size) { pending_ -= size; }

private:
    // ファームウェアが送出したバイトのうち、まだホストが回収していない量。
    std::size_t pending_ = 0;
};

}
