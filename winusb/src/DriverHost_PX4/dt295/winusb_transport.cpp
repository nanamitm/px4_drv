// SPDX-License-Identifier: GPL-2.0-only
#include "winusb_transport.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <array>
#include <cstring>

namespace px4::dt295 {
namespace {
int Error(DWORD code) {
    switch(code) {
    case ERROR_OPERATION_ABORTED:return -ECANCELED;
    case ERROR_DEVICE_NOT_CONNECTED:case ERROR_NO_SUCH_DEVICE:return -ENODEV;
    case ERROR_TIMEOUT:case ERROR_SEM_TIMEOUT:return -ETIMEDOUT;
    case ERROR_INVALID_DATA:return -EPROTO;
    default:return -EIO;
    }
}
uint32_t Be32(const uint8_t *p) {return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
}
DWORD NativeControlIo::Submit(WINUSB_INTERFACE_HANDLE usb,WINUSB_SETUP_PACKET setup,PUCHAR data,ULONG size,ULONG &actual,OVERLAPPED &operation) {
    return WinUsb_ControlTransfer(usb,setup,data,size,&actual,&operation)?ERROR_SUCCESS:GetLastError();
}
DWORD NativeControlIo::Collect(WINUSB_INTERFACE_HANDLE usb,OVERLAPPED &operation,ULONG &actual) {
    return WinUsb_GetOverlappedResult(usb,&operation,&actual,TRUE)?ERROR_SUCCESS:GetLastError();
}
void NativeControlIo::Cancel(HANDLE file,OVERLAPPED &operation) noexcept {CancelIoEx(file,&operation);}
dt295_io Transport::Callbacks() { return {Control,Now,Delay,Observe,this,Mismatch,Snapshot}; }
int Transport::Snapshot(void *ctx,const void *data,size_t size) {
    auto &self=*static_cast<Transport *>(ctx);
    if(self.snapshot_path_.empty())return 0;
    FILE *file=nullptr;
    if(_wfopen_s(&file,self.snapshot_path_.c_str(),self.snapshot_started_?L"ab":L"wb") || !file)return -EIO;
    const bool good=fwrite(data,1,size,file)==size;
    const bool closed=fclose(file)==0;
    self.snapshot_started_=true;
    return good && closed?0:-EIO;
}
int Transport::Control(void *ctx,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t size,unsigned timeout)
{
    return static_cast<Transport *>(ctx)->Transfer(type,request,value,index,data,size,timeout);
}
uint32_t Transport::Now(void *) { return GetTickCount(); }
void Transport::Delay(void *ctx,unsigned ms) {
    auto &self=*static_cast<Transport *>(ctx);
    const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(self.deadline_-Clock::now()).count();
    if(left>0)WaitForSingleObject(self.removed_,static_cast<DWORD>((std::min<long long>)(left,ms)));
}
void Transport::Observe(void *,const uint8_t *request,const uint8_t *response,int rc) {
    if(rc || (response && Be32(response+8)))
        msg_err("mAgicVISION: command seq=%u op=%u command=%x result=%d status=%u detail=%d\n",
            Be32(request+4),Be32(request+8),Be32(request+12),rc,
            response?Be32(response+8):0,response?static_cast<int32_t>(Be32(response+20)):0);
}
void Transport::Mismatch(void *,uint32_t address,uint32_t expected,uint32_t observed) {
    msg_err("mAgicVISION: firmware readback mismatch address=%08x expected=%08x observed=%08x\n",address,expected,observed);
}
int Transport::Transfer(uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t size,unsigned timeout)
{
    // 応答不明の要求を再送すると二重実行になるため、通信失敗後は同じセッションを再利用しない。
    if(failed_)return failed_;
    if(WaitForSingleObject(removed_,0)!=WAIT_TIMEOUT){last_error_=ERROR_DEVICE_NOT_CONNECTED;return failed_=-ENODEV;}
    auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-Clock::now()).count();
    if(left<=0){last_error_=ERROR_TIMEOUT;return failed_=-ETIMEDOUT;}
    // WinUSBはOUTにも書込み可能なユーザーバッファを要求する。上流のconst表を直接渡すと998になる。
    // コピーとOVERLAPPEDは転送完了の回収まで同じスコープで保持する。
    std::array<uint8_t,64> outbound{};
    if(!(type&0x80)){
        if(size>outbound.size() || (size && !data))return -EINVAL;
        if(size)std::memcpy(outbound.data(),data,size);
        data=outbound.data();
    }
    OVERLAPPED operation{};
    operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!operation.hEvent){last_error_=GetLastError();return failed_=Error(last_error_);}
    WINUSB_SETUP_PACKET setup{type,request,value,index,size};
    ULONG actual=0;
    DWORD code=io_.Submit(usb_,setup,static_cast<PUCHAR>(data),size,actual,operation);
    if(code==ERROR_IO_PENDING) {
        HANDLE events[]{removed_,operation.hEvent};
        DWORD wait=WaitForMultipleObjects(2,events,FALSE,static_cast<DWORD>((std::min<long long>)(left,timeout)));
        if(wait!=WAIT_OBJECT_0+1) {
            code=wait==WAIT_OBJECT_0?ERROR_DEVICE_NOT_CONNECTED:wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
            io_.Cancel(file_,operation);
        } else code=ERROR_SUCCESS;
        // bufferとeventは、キャンセルを含む完了の回収が済むまで保持する。
        DWORD collected=io_.Collect(usb_,operation,actual);
        if(!code)code=collected;
    }
    CloseHandle(operation.hEvent);
    if(!code && actual!=size)code=ERROR_INVALID_DATA;
    if(code) {
        last_error_=code;failed_=Error(code);
        msg_err("mAgicVISION: USB control type=%02x request=%02x value=%04x index=%04x bytes=%u actual=%lu win32=%lu errno=%d\n",
            type,request,value,index,size,actual,code,failed_);
    }
    return failed_;
}
}
