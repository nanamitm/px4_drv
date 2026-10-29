// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <chrono>
#include <cstddef>

namespace px4::pxbcud {
// USB のタイミング調整値ではなく、復旧が実らない状態を終了させる運用上の上限。
// 手動の再選局は新しい受信ワーカーとなり、この予算も最初からになる。
//
// 一定時間内の回数ではなく、連続して実らなかった回数を数える。
// 高負荷による一過性の失敗は、復旧の直後から長く受信できるため、
// 回数で数えると正常に受信できていた時間が評価されず、受信全体を止めてしまう。
// 録画中の停止は再開操作まで全欠落になるため、短い欠落より損害が大きい。
class RecoveryBudget {
public:
    using Clock=std::chrono::steady_clock;
    // 復旧が連続して実らない場合に諦める回数。
    static constexpr std::size_t Limit=8;
    // 直前のストリームがこの時間を超えて受信できていれば、連続失敗を忘れる。
    // 実測では、負荷による失敗の後でも 23.5秒・44分・59分と受信が続いたのに対し、
    // 復旧が実らない場合の再発は 221ms・277ms・369ms・448ms だった。
    // 2桁離れた両者の間に入る値を選ぶ。
    static constexpr auto HealthyRun=std::chrono::seconds(5);
    // 復旧手順は capture 停止・alt0・alt1・capture 開始の USB 制御転送を伴う。
    // 実らない再試行を詰めて繰り返すと同じブリッジを占有し続けるため、
    // 連続失敗のたびに間隔を広げる。1回目は欠落を最小にするため待たない。
    static constexpr auto BackoffStep=std::chrono::milliseconds(250);
    static constexpr auto BackoffMax=std::chrono::milliseconds(2000);

    // 直前のストリームが受信できていた時間を渡す。Take より先に呼ぶ。
    void NoteStream(Clock::duration ran) noexcept
    {
        if (ran>=HealthyRun) consecutive_=0;
    }
    bool Take() noexcept
    {
        if (consecutive_>=Limit) return false;
        ++consecutive_;
        return true;
    }
    // Take の後に呼び、次の復旧を始めるまでの待機時間を得る。
    std::chrono::milliseconds Backoff() const noexcept
    {
        if (consecutive_<=1) return std::chrono::milliseconds::zero();
        const auto wait=BackoffStep*static_cast<long long>(consecutive_-1);
        return wait<BackoffMax?wait:BackoffMax;
    }
    std::size_t Consecutive() const noexcept {return consecutive_;}
private:
    std::size_t consecutive_=0;
};
}
