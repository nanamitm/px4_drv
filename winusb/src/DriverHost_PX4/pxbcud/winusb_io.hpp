// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <process.h>
#include <windows.h>
#include <winusb.h>

namespace px4::pxbcud {
using IoClock = std::chrono::steady_clock;

struct WinUsbHandles {
    WinUsbHandles(HANDLE file, WINUSB_INTERFACE_HANDLE usb) noexcept : file(file), usb(usb) {}
    ~WinUsbHandles()
    {
        if (usb) WinUsb_Free(usb);
        if (file && file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    WinUsbHandles(const WinUsbHandles &) = delete;
    WinUsbHandles &operator=(const WinUsbHandles &) = delete;

    bool Ready() const noexcept { return deferred == 0; }
    HANDLE file;
    WINUSB_INTERFACE_HANDLE usb;
    std::atomic<unsigned> deferred{0};
};

struct PendingIo {
    PendingIo() = default;
    ~PendingIo() { if (operation.hEvent) CloseHandle(operation.hEvent); }
    PendingIo(const PendingIo &) = delete;
    PendingIo &operator=(const PendingIo &) = delete;

    void Cancel(HANDLE file) noexcept
    {
        if (pending) CancelIoEx(file, &operation);
    }

    bool WaitUntil(IoClock::time_point deadline) noexcept
    {
        if (!pending)
            return true;
        DWORD timeout = INFINITE;
        if (deadline != (IoClock::time_point::max)()) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - IoClock::now()).count();
            timeout = static_cast<DWORD>(std::clamp<long long>(remaining, 0, INFINITE - 1));
        }
        if (WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0)
            return false;
        pending = false;
        return true;
    }

    OVERLAPPED operation{};
    bool pending = false;
};

template<class Batch>
unsigned __stdcall ReapUsbRequests(void *context) noexcept
{
    auto *batch = static_cast<Batch *>(context);
    if (batch->WaitUntil((IoClock::time_point::max)())) {
        const auto handles = batch->handles;
        // 登録バッファの解除を終えてから同じ個体の新しい操作を許可する。
        delete batch;
        --handles->deferred;
    }
    return 0;
}

template<class Batch>
void CancelUsbRequests(std::unique_ptr<Batch> batch) noexcept
{
    // MSi2500 と同様、全要求に先に取消を送り、回収期限を共有する。
    const auto deadline = IoClock::now() + std::chrono::seconds(1);
    batch->CancelAll();
    if (batch->WaitUntil(deadline))
        return;

    ++batch->handles->deferred;
    auto *pending = batch.release();
    const auto thread = _beginthreadex(nullptr, 0, ReapUsbRequests<Batch>, pending, 0, nullptr);
    if (thread)
        CloseHandle(reinterpret_cast<HANDLE>(thread));
    else
        OutputDebugStringW(L"PX-BCUD: cannot start USB completion waiter; retaining pending requests\n");
}
} // namespace px4::pxbcud
