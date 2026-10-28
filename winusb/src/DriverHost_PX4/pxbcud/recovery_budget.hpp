// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <chrono>
#include <cstddef>

namespace px4::pxbcud {
// USB のタイミング調整値ではなく、故障中の再起動を制限する運用上の上限。
// 手動の再選局は新しい受信ワーカーとなり、この予算も最初からになる。
class RecoveryBudget {
public:
    using Clock=std::chrono::steady_clock;
    static constexpr std::size_t Limit=3;
    static constexpr auto Window=std::chrono::seconds(60);
    bool Take(Clock::time_point now) noexcept
    {
        std::size_t retained=0;
        for (std::size_t i=0;i<count_;++i)
            if (now-attempts_[i]<Window) attempts_[retained++]=attempts_[i];
        count_=retained;
        if (count_==Limit) return false;
        attempts_[count_++]=now;
        return true;
    }
private:
    std::array<Clock::time_point,Limit> attempts_{};
    std::size_t count_=0;
};
}
