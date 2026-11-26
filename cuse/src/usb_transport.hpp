// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "dt295/control_transport.hpp"
#include <libusb.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <mutex>

namespace px4::cuse {
// USB の切断を待機中のスレッドへ知らせる。制御転送の待機とカード操作の待機が、切断で直ちに戻るようにする。
class RemovalSignal {
public:
    void Set()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            removed_ = true;
        }
        changed_.notify_all();
    }
    bool Removed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return removed_;
    }
    // milliseconds 待つ。切断されたら -ENODEV、待ち終えたら 0 を返す。
    int Wait(unsigned milliseconds);
    // until まで待つ。torne の復調器の待機はミリ秒より短い単位で指定されるため、時刻で受け取る。
    int WaitUntil(std::chrono::steady_clock::time_point until);

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool removed_ = false;
};

// libusb の同期制御転送で dt295_io を実装する。WinUSB 版の Transport と同じ契約で、
// 失敗した要求は再送せず、Failed() が 0 以外の間は後続の転送を始めない。制御ロックの下で使う。
class UsbTransport final : public px4::dt295::ControlTransport {
public:
    using Clock = std::chrono::steady_clock;
    UsbTransport(libusb_device_handle *handle, RemovalSignal &removal) : handle_(handle), removal_(removal) {}
    void Begin(unsigned milliseconds) override { deadline_ = Clock::now() + std::chrono::milliseconds(milliseconds); }
    void BeginRestore(unsigned milliseconds) override
    {
        failed_ = 0;
        Begin(milliseconds);
    }
    void Block() { blocked_ = true; }
    int Failed() const override { return blocked_ ? -EIO : failed_; }
    void ClearFailure() override { failed_ = 0; }
    dt295_io Callbacks() override { return {Control, Now, Delay, nullptr, this, nullptr, nullptr}; }
    void ReportTiming(const char *stage) override;
    // libusb のエラー番号 (LIBUSB_ERROR_*) の絶対値。
    unsigned long LastError() const override { return last_error_; }

private:
    static int Control(void *ctx, std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                       void *data, std::uint16_t size, unsigned timeout);
    static std::uint32_t Now(void *);
    static void Delay(void *ctx, unsigned milliseconds);
    int Transfer(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, void *data,
                 std::uint16_t size, unsigned timeout);
    libusb_device_handle *handle_;
    RemovalSignal &removal_;
    Clock::time_point deadline_{};
    int failed_ = 0;
    std::atomic_bool blocked_{false};
    unsigned long last_error_ = 0;
    std::uint64_t transfers_ = 0, control_us_ = 0, max_control_us_ = 0;
};
}
