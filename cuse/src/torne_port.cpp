// SPDX-License-Identifier: GPL-2.0-only
#include "torne_port.hpp"
#include "msg.h"
#include <algorithm>

namespace px4::cuse {
namespace {
int UsbError(int rc)
{
    return rc == LIBUSB_ERROR_NO_DEVICE ? -ENODEV : rc == LIBUSB_ERROR_TIMEOUT ? -ETIMEDOUT
         : rc == LIBUSB_ERROR_PIPE      ? -EPIPE  : -EIO;
}

// 残り時間をミリ秒へ切り上げる。切り捨てると期限より早く打ち切るため。
unsigned TimeoutMs(TornePort::Clock::duration left)
{
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<unsigned>(std::clamp<long long>(ms, 1, 0x7fffffff));
}
}

int TornePort::Transfer(const torne::MemoryRequest &request, std::uint8_t *data, std::size_t length,
                        std::size_t &transferred)
{
    transferred = 0;
    if (!data || !length || length > 64)
        return -EINVAL;
    if (removal_.Removed())
        return -ENODEV;
    const auto left = deadline_ - Clock::now();
    if (left <= Clock::duration::zero())
        return -ETIMEDOUT;
    const auto begin = Clock::now();
    const int rc = libusb_control_transfer(handle_, request.type, request.request, request.value, request.index, data,
                                           static_cast<std::uint16_t>(length), TimeoutMs(left));
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
    {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        ++timing_.transfers;
        timing_.control_us += elapsed;
        timing_.max_control_us = std::max(timing_.max_control_us, elapsed);
    }
    if (rc < 0) {
        msg_err("%s: USB control type=%02x request=%02x value=%04x index=%04x bytes=%zu rc=%d\n", label_.c_str(),
                request.type, request.request, request.value, request.index, length, rc);
        return UsbError(rc);
    }
    // 短い転送は MemoryAccess が転送長で判定する。
    transferred = static_cast<std::size_t>(rc);
    return 0;
}

int TornePort::TransferPipe(std::uint8_t endpoint, std::uint8_t *data, std::size_t length, std::size_t &transferred,
                            Clock::time_point deadline)
{
    transferred = 0;
    if (!data || !length || length > 512)
        return -EINVAL;
    if (removal_.Removed())
        return -ENODEV;
    const auto left = (std::min)(deadline, deadline_) - Clock::now();
    if (left <= Clock::duration::zero())
        return -ETIMEDOUT;
    int actual = 0;
    // 応答の EP 0x83 は Interrupt、コマンドの EP 0x02 は Bulk (cech-zd1j-design.md)。
    const int rc = (endpoint & 0x80)
                       ? libusb_interrupt_transfer(handle_, endpoint, data, static_cast<int>(length), &actual, TimeoutMs(left))
                       : libusb_bulk_transfer(handle_, endpoint, data, static_cast<int>(length), &actual, TimeoutMs(left));
    {
        std::lock_guard<std::mutex> lock(timing_mutex_);
        ++timing_.pipes;
    }
    if (rc < 0) {
        // 期限切れは呼び出し側が I2C の timeout として扱う。部分的に届いたデータは使わない。
        if (rc != LIBUSB_ERROR_TIMEOUT)
            msg_err("%s: USB pipe endpoint=0x%02x bytes=%zu rc=%d\n", label_.c_str(), endpoint, length, rc);
        return UsbError(rc);
    }
    transferred = static_cast<std::size_t>(actual);
    return 0;
}

int TornePort::Pause(unsigned milliseconds)
{
    if (removal_.Removed())
        return -ECANCELED;
    const auto now = Clock::now();
    if (now >= deadline_)
        return -ETIMEDOUT;
    const auto end = (std::min)(deadline_, now + std::chrono::milliseconds(milliseconds));
    if (removal_.WaitUntil(end))
        return -ECANCELED;
    return Clock::now() >= deadline_ ? -ETIMEDOUT : 0;
}

torne::FrontendSignal::State TornePort::Poll(bool, int &) noexcept
{
    return removal_.Removed() ? State::removed : State::clear;
}

// WinUSB 版の WaitFrontendDelay と同じく、待つ前と後に抜去と期限を確かめ、期限を越えては待たない。
int TornePort::Sleep(bool, Clock::time_point deadline, std::int64_t ns) noexcept
{
    const auto check = [this, deadline] {
        if (removal_.Removed())
            return -ECANCELED;
        return Clock::now() >= deadline ? -ETIMEDOUT : 0;
    };
    if (const int error = check())
        return error;
    if (ns <= 0)
        return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
    if (remaining <= 0)
        return -ETIMEDOUT;
    removal_.WaitUntil(Clock::now() + std::chrono::nanoseconds((std::min)(ns, static_cast<std::int64_t>(remaining))));
    return check();
}

void TornePort::ReportTiming(const char *stage)
{
    Timing snapshot;
    {
        // Session の制御ロックの外から報告するため、カード操作による更新と同期する。
        std::lock_guard<std::mutex> lock(timing_mutex_);
        snapshot = timing_;
        timing_ = {};
    }
    msg_info("%s: transport stage=%s transfers=%llu pipes=%llu mean_control_us=%llu max_control_us=%llu\n",
             label_.c_str(), stage, static_cast<unsigned long long>(snapshot.transfers),
             static_cast<unsigned long long>(snapshot.pipes),
             static_cast<unsigned long long>(snapshot.transfers ? snapshot.control_us / snapshot.transfers : 0),
             static_cast<unsigned long long>(snapshot.max_control_us));
}
}
