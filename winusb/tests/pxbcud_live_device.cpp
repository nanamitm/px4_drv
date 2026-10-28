// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/pxbcud/device.hpp"
#include <atomic>
#include <array>
#include <algorithm>

namespace {
std::atomic<DWORD> injected_error{ERROR_SUCCESS};
std::atomic<DWORD> observed_error{ERROR_SUCCESS};
std::atomic<unsigned> remaining_failures{0}, observed_failures{0};
std::atomic<unsigned> started_streams{0};
thread_local std::array<LPOVERLAPPED,5> isoch_operations{};

BOOL TestReadIsochPipeAsap(WINUSB_ISOCH_BUFFER_HANDLE buffer, ULONG offset,
    ULONG length, BOOL continuous, ULONG count,
    PUSBD_ISO_PACKET_DESCRIPTOR packets, LPOVERLAPPED operation)
{
    const BOOL result=WinUsb_ReadIsochPipeAsap(buffer,offset,length,continuous,count,packets,operation);
    const DWORD error=result?ERROR_SUCCESS:GetLastError();
    if (result || error==ERROR_IO_PENDING) {
        const auto slot=std::find(isoch_operations.begin(),isoch_operations.end(),nullptr);
        if (slot!=isoch_operations.end()) *slot=operation;
        if (!continuous) ++started_streams;
    }
    if (!result) SetLastError(error);
    return result;
}

BOOL TestGetOverlappedResult(WINUSB_INTERFACE_HANDLE usb, LPOVERLAPPED operation,
    LPDWORD bytes, BOOL wait)
{
    const BOOL result=WinUsb_GetOverlappedResult(usb,operation,bytes,wait);
    const DWORD result_error=result?ERROR_SUCCESS:GetLastError();
    const auto slot=std::find(isoch_operations.begin(),isoch_operations.end(),operation);
    const bool isoch=slot!=isoch_operations.end();
    if (isoch && (result || result_error!=ERROR_IO_INCOMPLETE)) *slot=nullptr;
    // 実際の完了を回収してから失敗を模擬するため、未完了 I/O の寿命や
    // 制御転送には触れず、受信エラー後の再選局だけを決定的に検証できる。
    if (result && isoch && !wait) {
        const DWORD error=injected_error.load();
        if (error && remaining_failures.load()) {
            --remaining_failures;
            ++observed_failures;
            observed_error=error;
            SetLastError(error);
            return FALSE;
        }
    }
    if (!result) SetLastError(result_error);
    return result;
}
}

void InjectPxBcudCompletionFailure(DWORD error,unsigned count)
{
    observed_error=ERROR_SUCCESS;
    observed_failures=0;
    injected_error=error;
    remaining_failures=count;
}
DWORD ObservedPxBcudCompletionFailure() { return observed_error.load(); }
unsigned ObservedPxBcudFailureCount() { return observed_failures.load(); }
unsigned ObservedPxBcudStreamCount() { return started_streams.load(); }

// この翻訳単位は実機試験ツールだけが使用し、配布用ホストは元の
// device.cpp を直接ビルドする。通常モードでは API の結果をそのまま返す。
#define WinUsb_ReadIsochPipeAsap TestReadIsochPipeAsap
#define WinUsb_GetOverlappedResult TestGetOverlappedResult
#include "../src/DriverHost_PX4/pxbcud/device.cpp"
