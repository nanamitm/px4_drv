// SPDX-License-Identifier: GPL-2.0-only
#include "usb_transport.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>

namespace px4::cuse {
int RemovalSignal::Wait(unsigned milliseconds)
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return removed_; });
    return removed_ ? -ENODEV : 0;
}

int RemovalSignal::WaitUntil(std::chrono::steady_clock::time_point until)
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_until(lock, until, [this] { return removed_; });
    return removed_ ? -ENODEV : 0;
}

int UsbTransport::Control(void *ctx, std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                          void *data, std::uint16_t size, unsigned timeout)
{
    return static_cast<UsbTransport *>(ctx)->Transfer(type, request, value, index, data, size, timeout);
}

std::uint32_t UsbTransport::Now(void *)
{
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

void UsbTransport::Delay(void *ctx, unsigned milliseconds)
{
    auto &self = *static_cast<UsbTransport *>(ctx);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(self.deadline_ - Clock::now()).count();
    if (left > 0)
        self.removal_.Wait(static_cast<unsigned>(std::min<long long>(left, milliseconds)));
}

void UsbTransport::ReportTiming(const char *stage)
{
    msg_info("PIX-DT295: transport stage=%s transfers=%llu mean_control_us=%llu max_control_us=%llu\n", stage,
             static_cast<unsigned long long>(transfers_),
             static_cast<unsigned long long>(transfers_ ? control_us_ / transfers_ : 0),
             static_cast<unsigned long long>(max_control_us_));
    transfers_ = control_us_ = max_control_us_ = 0;
}

int UsbTransport::Transfer(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                           void *data, std::uint16_t size, unsigned timeout)
{
    // 応答不明の要求を再送すると二重実行になるため、通信失敗後は同じセッションを再利用しない。
    if (const int error = Failed())
        return error;
    if (removal_.Removed()) {
        last_error_ = static_cast<unsigned long>(-LIBUSB_ERROR_NO_DEVICE);
        return failed_ = -ENODEV;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now()).count();
    if (left <= 0) {
        last_error_ = static_cast<unsigned long>(-LIBUSB_ERROR_TIMEOUT);
        return failed_ = -ETIMEDOUT;
    }
    const auto begin = Clock::now();
    const int rc = libusb_control_transfer(handle_, type, request, value, index, static_cast<unsigned char *>(data),
                                           size, static_cast<unsigned>(std::min<long long>(left, timeout)));
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
    ++transfers_;
    control_us_ += elapsed;
    max_control_us_ = std::max(max_control_us_, elapsed);
    if (rc == size)
        return 0;
    last_error_ = rc < 0 ? static_cast<unsigned long>(-rc) : 0;
    failed_ = rc == LIBUSB_ERROR_NO_DEVICE ? -ENODEV
            : rc == LIBUSB_ERROR_TIMEOUT   ? -ETIMEDOUT
            : rc >= 0                      ? -EPROTO
                                           : -EIO;
    msg_err("PIX-DT295: USB control type=%02x request=%02x value=%04x index=%04x bytes=%u rc=%d errno=%d\n", type,
            request, value, index, size, rc, failed_);
    return failed_;
}
}
