// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "runtime.h"
#include <windows.h>
#include <winusb.h>
#include <chrono>
#include <string>

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
// 1個体の制御ロック下で使用。TSのOVERLAPPEDは別スレッドが所有する。
class Transport {
public:
    using Clock=std::chrono::steady_clock;
    Transport(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE removed,ControlIo *io=nullptr)
        :file_(file),removed_(removed),usb_(usb),io_(io?*io:native_) {}
    void Begin(unsigned milliseconds) { deadline_=Clock::now()+std::chrono::milliseconds(milliseconds); }
    dt295_io Callbacks();
    DWORD LastError() const { return last_error_; }
    void SetSnapshotPath(std::wstring path) { snapshot_path_=std::move(path);snapshot_started_=false; }
private:
    static int Control(void *,uint8_t,uint8_t,uint16_t,uint16_t,void *,uint16_t,unsigned);
    static uint32_t Now(void *);
    static void Delay(void *,unsigned);
    static void Observe(void *,const uint8_t *,const uint8_t *,int);
    static void Mismatch(void *,uint32_t,uint32_t,uint32_t);
    static int Snapshot(void *,const void *,size_t);
    int Transfer(uint8_t,uint8_t,uint16_t,uint16_t,void *,uint16_t,unsigned);
    HANDLE file_,removed_;
    WINUSB_INTERFACE_HANDLE usb_;
    NativeControlIo native_;
    ControlIo &io_;
    Clock::time_point deadline_{};
    DWORD last_error_=0;
    int failed_=0;
    std::wstring snapshot_path_;
    bool snapshot_started_=false;
};
}
