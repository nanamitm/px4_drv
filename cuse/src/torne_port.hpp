// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// WinUSB 版と共有する torne::Session へ渡す、USB の転送と待機の libusb 版。
// WinUSB 版の WinUsbSessionPort と同じ契約で、期限は操作全体に 1 回だけ設定し、失敗した要求は再送しない。
// PTX の ioctl には受信機の停止要求がないため、停止は扱わず、USB の切断だけで待機と転送を中断する。
#include "usb_transport.hpp"
#include "torne/session.hpp"
#include <libusb.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>

namespace px4::cuse {
class TornePort final : public torne::SessionPort,
                        private torne::ControlTransport,
                        private torne::BootstrapWait,
                        private torne::FrontendSignal {
public:
    using Clock = std::chrono::steady_clock;
    TornePort(libusb_device_handle *handle, RemovalSignal &removal, std::string label)
        : handle_(handle), removal_(removal), label_(std::move(label)) {}

    torne::ControlTransport &Transport() noexcept override { return *this; }
    torne::BootstrapWait &Wait() noexcept override { return *this; }
    torne::FrontendSignal &Signal() noexcept override { return *this; }
    void Begin(Clock::time_point deadline, bool) noexcept override { deadline_ = deadline; }
    int TransferPipe(std::uint8_t endpoint, std::uint8_t *data, std::size_t length, std::size_t &transferred,
                     Clock::time_point deadline) override;
    bool TimedOut(int error) const noexcept override { return error == -ETIMEDOUT; }
    // 前回の報告以降の制御転送の回数と所要時間を記録する (検証環境の往復時間と選局時間の関係を残すため)。
    void ReportTiming(const char *stage);

private:
    int Transfer(const torne::MemoryRequest &request, std::uint8_t *data, std::size_t length,
                 std::size_t &transferred) override;
    int Pause(unsigned milliseconds) override;
    State Poll(bool stoppable, int &error) noexcept override;
    bool StopRequested() noexcept override { return false; }
    int Sleep(bool stoppable, Clock::time_point deadline, std::int64_t ns) noexcept override;
    bool Aborted(int error) const noexcept override { return error == -ECANCELED; }
    int AbortError() const noexcept override { return -ECANCELED; }

    libusb_device_handle *handle_;
    RemovalSignal &removal_;
    const std::string label_;
    Clock::time_point deadline_ = (Clock::time_point::min)();
    std::uint64_t transfers_ = 0, control_us_ = 0, max_control_us_ = 0, pipes_ = 0;
};
}
