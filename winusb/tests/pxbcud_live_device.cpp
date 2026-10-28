// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/pxbcud/device.hpp"
#include <atomic>

namespace {
std::atomic<DWORD> injected_error{ERROR_SUCCESS};
std::atomic<DWORD> observed_error{ERROR_SUCCESS};
thread_local bool isoch_worker=false;

BOOL TestReadIsochPipeAsap(WINUSB_ISOCH_BUFFER_HANDLE buffer, ULONG offset,
    ULONG length, BOOL continuous, ULONG count,
    PUSBD_ISO_PACKET_DESCRIPTOR packets, LPOVERLAPPED operation)
{
    isoch_worker=true;
    return WinUsb_ReadIsochPipeAsap(buffer,offset,length,continuous,count,packets,operation);
}

BOOL TestGetOverlappedResult(WINUSB_INTERFACE_HANDLE usb, LPOVERLAPPED operation,
    LPDWORD bytes, BOOL wait)
{
    const BOOL result=WinUsb_GetOverlappedResult(usb,operation,bytes,wait);
    // 実際の完了を回収してから失敗を模擬するため、未完了 I/O の寿命や
    // 制御転送には触れず、受信エラー後の再選局だけを決定的に検証できる。
    if (result && isoch_worker) {
        const DWORD error=injected_error.exchange(ERROR_SUCCESS);
        if (error) {
            observed_error=error;
            SetLastError(error);
            return FALSE;
        }
    }
    return result;
}
}

void InjectPxBcudCompletionFailure(DWORD error)
{
    observed_error=ERROR_SUCCESS;
    injected_error=error;
}
DWORD ObservedPxBcudCompletionFailure() { return observed_error.load(); }

// この翻訳単位は実機試験ツールだけが使用し、配布用ホストは元の
// device.cpp を直接ビルドする。通常モードでは API の結果をそのまま返す。
#define WinUsb_ReadIsochPipeAsap TestReadIsochPipeAsap
#define WinUsb_GetOverlappedResult TestGetOverlappedResult
#include "../src/DriverHost_PX4/pxbcud/device.cpp"
