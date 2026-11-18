// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/pxbcud/device.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace {
std::atomic<DWORD> injected_error{ERROR_SUCCESS};
std::atomic<DWORD> observed_error{ERROR_SUCCESS};
std::atomic<unsigned> remaining_failures{0}, observed_failures{0};
std::atomic<unsigned> started_streams{0};
thread_local std::array<LPOVERLAPPED,5> isoch_operations{};
// PAT を壊す期限 (steady_clock の値) と、TEI を立てた PAT パケットの数。
std::atomic<std::chrono::steady_clock::rep> pat_damage_until{0};
std::atomic<unsigned long long> damaged_pat_packets{0};
// 受信スレッドが登録したバッファーと、完了待ちの要求のパケット記述子。
struct Submitted { WINUSB_ISOCH_BUFFER_HANDLE buffer; PUSBD_ISO_PACKET_DESCRIPTOR packets; ULONG count; };
thread_local std::unordered_map<WINUSB_ISOCH_BUFFER_HANDLE,std::uint8_t*> isoch_buffers;
thread_local std::unordered_map<LPOVERLAPPED,Submitted> isoch_submitted;

BOOL TestRegisterIsochBuffer(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, PUCHAR data, ULONG size,
    PWINUSB_ISOCH_BUFFER_HANDLE handle)
{
    const BOOL result=WinUsb_RegisterIsochBuffer(usb,pipe,data,size,handle);
    if (result) isoch_buffers[*handle]=data;
    return result;
}

// 復旧直後に信号が悪く PAT の CRC が通らない状態を、PID 0 の TEI で模擬する。
// PlainFramer と同じく、正常な isoch パケットの中身を連結した列でヘッダーを探す。
void DamagePat(const Submitted &submitted)
{
    const auto found=isoch_buffers.find(submitted.buffer);
    if (found==isoch_buffers.end()) return;
    std::vector<std::uint8_t*> bytes;
    for (ULONG i=0;i<submitted.count;++i) {
        const auto &packet=submitted.packets[i];
        if (packet.Status || !packet.Length) continue;
        for (ULONG j=0;j<packet.Length;++j) bytes.push_back(found->second+packet.Offset+j);
    }
    for (std::size_t k=0;k+2<bytes.size();++k) {
        if (*bytes[k]==0x47 && !(*bytes[k+1]&0x1f) && !*bytes[k+2]) {
            *bytes[k+1]|=0x80;
            ++damaged_pat_packets;
        }
    }
}

BOOL TestReadIsochPipeAsap(WINUSB_ISOCH_BUFFER_HANDLE buffer, ULONG offset,
    ULONG length, BOOL continuous, ULONG count,
    PUSBD_ISO_PACKET_DESCRIPTOR packets, LPOVERLAPPED operation)
{
    const BOOL result=WinUsb_ReadIsochPipeAsap(buffer,offset,length,continuous,count,packets,operation);
    const DWORD error=result?ERROR_SUCCESS:GetLastError();
    if (result || error==ERROR_IO_PENDING) {
        const auto slot=std::find(isoch_operations.begin(),isoch_operations.end(),nullptr);
        if (slot!=isoch_operations.end()) *slot=operation;
        isoch_submitted[operation]={buffer,packets,count};
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
    const auto submitted=isoch_submitted.find(operation);
    if (submitted!=isoch_submitted.end() && (result || result_error!=ERROR_IO_INCOMPLETE)) {
        if (result && !wait && std::chrono::steady_clock::now().time_since_epoch().count()<pat_damage_until.load())
            DamagePat(submitted->second);
        isoch_submitted.erase(submitted);
    }
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
void DamagePxBcudPat(unsigned milliseconds)
{
    damaged_pat_packets=0;
    pat_damage_until=(std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds)).time_since_epoch().count();
}
unsigned long long DamagedPxBcudPatPackets() { return damaged_pat_packets.load(); }

// この翻訳単位は実機試験ツールだけが使用し、配布用ホストは元の
// device.cpp を直接ビルドする。通常モードでは API の結果をそのまま返す。
#define WinUsb_RegisterIsochBuffer TestRegisterIsochBuffer
#define WinUsb_ReadIsochPipeAsap TestReadIsochPipeAsap
#define WinUsb_GetOverlappedResult TestGetOverlappedResult
#include "../src/DriverHost_PX4/pxbcud/device.cpp"
