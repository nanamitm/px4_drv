// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <chrono>

namespace px4::torne {
enum class ReceptionFault { none, stalled, lock_lost };
class ReceptionWatch {
public:
    using Clock=std::chrono::steady_clock;
    explicit ReceptionWatch(Clock::time_point now):last_ts_(now){}
    void Received(Clock::time_point now) noexcept {last_ts_=now;}
    void Lock(bool locked,Clock::time_point now) noexcept {
        if(locked)lost_=false;
        else if(!lost_){lost_=true;lost_at_=now;}
    }
    ReceptionFault Fault(Clock::time_point now) const noexcept {
        if(now-last_ts_>=std::chrono::seconds(5))return ReceptionFault::stalled;
        if(lost_ && now-lost_at_>=std::chrono::seconds(5))return ReceptionFault::lock_lost;
        return ReceptionFault::none;
    }
private:
    Clock::time_point last_ts_,lost_at_{};
    bool lost_=false;
};

enum class RecoveryResult { ready, no_lock, fatal, stopped };
template<class Now,class Poll,class Wait> RecoveryResult WaitInitialLock(ReceptionWatch::Clock::time_point deadline,Now now,Poll poll,Wait wait) {
    while(now()<deadline){
        auto result=poll();
        if(result==RecoveryResult::fatal || result==RecoveryResult::stopped)return result;
        if(now()>=deadline)return RecoveryResult::no_lock;
        if(result==RecoveryResult::ready)return result;
        const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-now()).count();
        if(remaining<=0)break;
        if(!wait(static_cast<unsigned>(remaining<100?remaining:100)))return RecoveryResult::stopped;
    }
    return RecoveryResult::no_lock;
}
// 初回は直ちに試し、無信号だけを 3 秒間隔で再試行する。USB/制御失敗は
// 呼び出し側が fatal とし、電源再投入や別個体への割り当てをここへ持ち込まない。
template<class Attempt,class Wait> RecoveryResult RetryReception(Attempt attempt,Wait wait) {
    if(!wait(0))return RecoveryResult::stopped;
    for(;;){
        auto result=attempt();
        if(result!=RecoveryResult::no_lock)return result;
        if(!wait(3000))return RecoveryResult::stopped;
    }
}
}
