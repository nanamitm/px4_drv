// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// FrontendSignal の WinUSB 版の実装。抜去 (removed) と受信機の停止要求 (stop) は手動リセットのイベントで、
// 所有者が両方のイベントをこのオブジェクトより長く保持する。
#include "frontend_signal.hpp"
#include "frontend_wait.hpp"

namespace px4::torne {
class WinFrontendSignal final : public FrontendSignal {
public:
    WinFrontendSignal(HANDLE removed, HANDLE stop) noexcept : removed_(removed), stop_(stop) {}
    State Poll(bool stoppable, int &error) noexcept override
    {
        const HANDLE events[]{removed_, stop_};
        const bool with_stop = stoppable && stop_;
        const auto state = WaitForMultipleObjects(with_stop ? 2 : 1, events, FALSE, 0);
        if (state == WAIT_TIMEOUT) return State::clear;
        if (state == WAIT_OBJECT_0) return State::removed;
        if (with_stop && state == WAIT_OBJECT_0 + 1) return State::stopped;
        error = static_cast<int>(state == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED);
        return State::failed;
    }
    bool StopRequested() noexcept override { return stop_ && WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0; }
    int Sleep(bool stoppable, std::chrono::steady_clock::time_point deadline, std::int64_t ns) noexcept override
    {
        return static_cast<int>(WaitFrontendDelay(removed_, deadline, ns, stoppable ? stop_ : nullptr));
    }
    bool Aborted(int error) const noexcept override { return error == ERROR_OPERATION_ABORTED; }
    int AbortError() const noexcept override { return ERROR_OPERATION_ABORTED; }

private:
    HANDLE removed_, stop_;
};
} // namespace px4::torne
