// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "control_transport.hpp"
#include "runtime.h"
#include <windows.h>
#include <winusb.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace px4::dt295 {
class ControlIo {
public:
    virtual ~ControlIo()=default;
    virtual DWORD Submit(WINUSB_INTERFACE_HANDLE,WINUSB_SETUP_PACKET,PUCHAR,ULONG,ULONG &,OVERLAPPED &)=0;
    virtual DWORD Collect(WINUSB_INTERFACE_HANDLE,OVERLAPPED &,ULONG &)=0;
    virtual void Cancel(HANDLE,OVERLAPPED &) noexcept=0;
};
class NativeControlIo final : public ControlIo {
public:
    DWORD Submit(WINUSB_INTERFACE_HANDLE,WINUSB_SETUP_PACKET,PUCHAR,ULONG,ULONG &,OVERLAPPED &) override;
    DWORD Collect(WINUSB_INTERFACE_HANDLE,OVERLAPPED &,ULONG &) override;
    void Cancel(HANDLE,OVERLAPPED &) noexcept override;
};
// 期限後も未完了の要求がバッファと USB ハンドルを所有する。
struct UsbHandles {
    UsbHandles(HANDLE file, WINUSB_INTERFACE_HANDLE usb) noexcept : file(file), usb(usb) {}
    ~UsbHandles();
    UsbHandles(const UsbHandles &) = delete;
    UsbHandles &operator=(const UsbHandles &) = delete;
    bool Ready() const noexcept { return deferred == 0; }

    const HANDLE file;
    const WINUSB_INTERFACE_HANDLE usb;
    std::atomic<unsigned> deferred{0};
};

struct UsbRequest {
    explicit UsbRequest(std::size_t size);
    ~UsbRequest();
    UsbRequest(const UsbRequest &) = delete;
    UsbRequest &operator=(const UsbRequest &) = delete;
    bool Reset() noexcept;
    bool Wait(DWORD timeout) noexcept;

    OVERLAPPED operation{};
    ULONG submitted_bytes = 0;
    std::vector<std::uint8_t> buffer;
    bool pending = false;
};

struct UsbRequests {
    explicit UsbRequests(const std::shared_ptr<UsbHandles> &usb) noexcept : usb(usb) {}
    const std::shared_ptr<UsbHandles> usb;
    std::vector<std::unique_ptr<UsbRequest>> requests;
};

// 一括でキャンセルし、全要求を共通の期限まで待つ。未完了分は完了待ちのスレッドへ渡す。
void DrainUsbRequests(std::unique_ptr<UsbRequests> &batch, ControlIo *control = nullptr) noexcept;

// 1個体の制御ロック下で使用。TSのOVERLAPPEDは別スレッドが所有する。
class Transport final : public ControlTransport {
public:
    using Clock=std::chrono::steady_clock;
    Transport(const std::shared_ptr<UsbHandles> &usb, HANDLE removed, ControlIo *io = nullptr)
        : usb_(usb), removed_(removed), io_(io ? *io : native_) {}
    void Begin(unsigned milliseconds) override { deadline_=Clock::now()+std::chrono::milliseconds(milliseconds); }
    // 失敗した要求を再送せず、保存済み前像を戻すときだけ通信を再開する。
    void BeginRestore(unsigned milliseconds) override { failed_=0; Begin(milliseconds); }
    int Failed() const override { return failed_; }
    // カード操作のように、失敗した要求の結果へ後続が依存しない操作の終わりに、呼び出し元が失敗を解除する。
    // 失敗した要求そのものは再送しない。
    void ClearFailure() override { failed_=0; }
    dt295_io Callbacks() override;
    unsigned long LastError() const override { return last_error_; }
    void ReportTiming(const char *stage) override;
private:
    static int Control(void *,uint8_t,uint8_t,uint16_t,uint16_t,void *,uint16_t,unsigned);
    static uint32_t Now(void *);
    static void Delay(void *,unsigned);
    int Transfer(uint8_t,uint8_t,uint16_t,uint16_t,void *,uint16_t,unsigned);
    const std::shared_ptr<UsbHandles> usb_;
    HANDLE removed_;
    std::unique_ptr<UsbRequests> control_;
    NativeControlIo native_;
    ControlIo &io_;
    Clock::time_point deadline_{};
    DWORD last_error_=0;
    int failed_=0;
    uint64_t transfers_=0,control_us_=0,max_control_us_=0,in_=0,out_=0;
    uint64_t in_us_=0,out_us_=0;
    uint64_t delays_=0,requested_delay_us_=0,delay_us_=0;
};
}
