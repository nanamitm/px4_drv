// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// SessionPort の WinUSB 版の実装。抜去 (removed) と受信機の停止要求 (stop) は手動リセットのイベントで、
// 所有者が両方のイベントと WinUSB のハンドルを Session より長く保持する。
#include "frontend_signal_win32.hpp"
#include "session.hpp"
#include "winusb_control.hpp"

namespace px4::torne {
class WinUsbSessionPort final : public SessionPort {
public:
    WinUsbSessionPort(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE removed, HANDLE stop) noexcept
        : stop_(stop), control_(file, usb, removed, io_), signal_(removed, stop) {}
    ControlTransport &Transport() noexcept override { return control_; }
    BootstrapWait &Wait() noexcept override { return control_; }
    FrontendSignal &Signal() noexcept override { return signal_; }
    void Begin(Clock::time_point deadline, bool stoppable) noexcept override
    {
        control_.Begin(deadline, stoppable ? stop_ : nullptr);
    }
    int TransferPipe(std::uint8_t endpoint, std::uint8_t *data, std::size_t length,
        std::size_t &transferred, Clock::time_point deadline) override
    {
        return control_.TransferPipe(endpoint, data, length, transferred, deadline);
    }
    bool TimedOut(int error) const noexcept override { return error == ERROR_TIMEOUT || error == ERROR_SEM_TIMEOUT; }

private:
    HANDLE stop_;
    NativeControlIo io_;
    WinUsbControl control_;
    WinFrontendSignal signal_;
};
} // namespace px4::torne
