// SPDX-License-Identifier: GPL-2.0-only
#include "winusb_transport.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <process.h>

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
}
DWORD NativeControlIo::Submit(WINUSB_INTERFACE_HANDLE usb,WINUSB_SETUP_PACKET setup,PUCHAR data,ULONG size,ULONG &actual,OVERLAPPED &operation) {
    return WinUsb_ControlTransfer(usb,setup,data,size,&actual,&operation)?ERROR_SUCCESS:GetLastError();
}
DWORD NativeControlIo::Collect(WINUSB_INTERFACE_HANDLE usb,OVERLAPPED &operation,ULONG &actual) {
    return WinUsb_GetOverlappedResult(usb,&operation,&actual,FALSE)?ERROR_SUCCESS:GetLastError();
}
void NativeControlIo::Cancel(HANDLE file,OVERLAPPED &operation) noexcept {CancelIoEx(file,&operation);}
UsbHandles::~UsbHandles()
{
    if (usb) WinUsb_Free(usb);
    if (file && file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

UsbRequest::UsbRequest(std::size_t size) : buffer(size)
{
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

UsbRequest::~UsbRequest()
{
    if (operation.hEvent) CloseHandle(operation.hEvent);
}

bool UsbRequest::Reset() noexcept
{
    const HANDLE event = operation.hEvent;
    operation = {};
    operation.hEvent = event;
    submitted_bytes = 0;
    return event && ResetEvent(event);
}

bool UsbRequest::Wait(DWORD timeout) noexcept
{
    // キャンセル後は転送結果を使わないため、完了イベントの通知で回収できる。
    if (pending && WaitForSingleObject(operation.hEvent, timeout) == WAIT_OBJECT_0)
        pending = false;
    return !pending;
}

namespace {
unsigned __stdcall ReapUsbRequests(void *context) noexcept
{
    auto *batch = static_cast<UsbRequests *>(context);
    for (const auto &request : batch->requests)
        if (!request->Wait(INFINITE))
            return 0;
    const auto usb = batch->usb;
    delete batch;
    --usb->deferred;
    return 0;
}
}

void DrainUsbRequests(std::unique_ptr<UsbRequests> &batch, ControlIo *control) noexcept
{
    if (!batch)
        return;
    const auto deadline = Transport::Clock::now() + std::chrono::seconds(1);
    for (const auto &request : batch->requests) {
        if (!request->pending)
            continue;
        if (control)
            control->Cancel(batch->usb->file, request->operation);
        else
            CancelIoEx(batch->usb->file, &request->operation);
    }

    bool done = true;
    for (const auto &request : batch->requests) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Transport::Clock::now());
        if (!request->Wait(static_cast<DWORD>((std::max)(0LL, remaining.count()))))
            done = false;
    }
    if (done)
        return;

    msg_warn("PIX-DT295: USB cancellation did not complete within 1 second\n");
    ++batch->usb->deferred;
    auto *pending = batch.release();
    const auto thread = _beginthreadex(nullptr, 0, ReapUsbRequests, pending, 0, nullptr);
    if (thread)
        CloseHandle(reinterpret_cast<HANDLE>(thread));
    else
        msg_err("PIX-DT295: cannot start USB completion waiter; retaining pending requests\n");
}

dt295_io Transport::Callbacks() { return {Control,Now,Delay,nullptr,this,nullptr,nullptr}; }
int Transport::Control(void *ctx,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t size,unsigned timeout)
{
    return static_cast<Transport *>(ctx)->Transfer(type,request,value,index,data,size,timeout);
}
uint32_t Transport::Now(void *) { return GetTickCount(); }
void Transport::Delay(void *ctx,unsigned ms) {
    auto &self=*static_cast<Transport *>(ctx);
    const auto begin=Clock::now();
    const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(self.deadline_-Clock::now()).count();
    if(left>0)WaitForSingleObject(self.removed_,static_cast<DWORD>((std::min<long long>)(left,ms)));
    ++self.delays_;self.requested_delay_us_+=static_cast<uint64_t>(ms)*1000;
    self.delay_us_+=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count());
}
void Transport::ReportTiming(const char *stage) {
    msg_info("PIX-DT295: transport stage=%s transfers=%llu in=%llu out=%llu in_us=%llu out_us=%llu control_us=%llu mean_control_us=%llu max_control_us=%llu delays=%llu requested_delay_us=%llu actual_delay_us=%llu\n",
        stage,transfers_,in_,out_,in_us_,out_us_,control_us_,transfers_?control_us_/transfers_:0,max_control_us_,
        delays_,requested_delay_us_,delay_us_);
    transfers_=control_us_=max_control_us_=in_=out_=delays_=requested_delay_us_=delay_us_=0;
    in_us_=out_us_=0;
}
int Transport::Transfer(uint8_t type,uint8_t request_code,uint16_t value,uint16_t index,void *data,uint16_t size,unsigned timeout)
{
    // 応答不明の要求を再送すると二重実行になるため、通信失敗後は同じセッションを再利用しない。
    if(failed_)return failed_;
    if(WaitForSingleObject(removed_,0)!=WAIT_TIMEOUT){last_error_=ERROR_DEVICE_NOT_CONNECTED;return failed_=-ENODEV;}
    auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-Clock::now()).count();
    if(left<=0){last_error_=ERROR_TIMEOUT;return failed_=-ETIMEDOUT;}
    if (!usb_->Ready()) {
        last_error_ = ERROR_IO_PENDING;
        return failed_ = -EIO;
    }
    const bool read = (type & 0x80) != 0;
    if ((!read && size > 64) || (size && !data))
        return -EINVAL;
    try {
        if (!control_) {
            auto batch = std::make_unique<UsbRequests>(usb_);
            batch->requests.push_back(std::make_unique<UsbRequest>(64));
            control_ = std::move(batch);
        }
        auto &buffer = control_->requests.front()->buffer;
        if (buffer.size() < size)
            buffer.resize(size);
    } catch (const std::bad_alloc &) {
        last_error_ = ERROR_NOT_ENOUGH_MEMORY;
        return failed_ = -ENOMEM;
    }
    auto &request = *control_->requests.front();
    if (!request.Reset()) {
        last_error_ = request.operation.hEvent ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        control_.reset();
        return failed_ = Error(last_error_);
    }
    // WinUSB の転送先は呼び出し元から借りず、完了まで確保する。OUT にも書込み可能な領域が必要。
    if (!read && size)
        std::memcpy(request.buffer.data(), data, size);
    WINUSB_SETUP_PACKET setup{type,request_code,value,index,size};
    const auto begin = Clock::now();
    DWORD code = io_.Submit(usb_->usb, setup, request.buffer.data(), size, request.submitted_bytes, request.operation);
    ULONG actual = code == ERROR_SUCCESS ? request.submitted_bytes : 0;
    if (code == ERROR_IO_PENDING) {
        request.pending = true;
        HANDLE events[]{removed_, request.operation.hEvent};
        const auto until = (std::min)(deadline_, begin + std::chrono::milliseconds(timeout));
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(until - Clock::now()).count();
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>((std::max)(0LL, remaining)));
        if (wait == WAIT_OBJECT_0 + 1) {
            code = io_.Collect(usb_->usb, request.operation, actual);
            request.pending = code == ERROR_IO_INCOMPLETE || code == ERROR_IO_PENDING;
        } else {
            code = wait == WAIT_OBJECT_0 ? ERROR_DEVICE_NOT_CONNECTED :
                   wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        }
        if (request.pending)
            DrainUsbRequests(control_, &io_);
    }
    const auto elapsed=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count());
    ++transfers_;if(type&0x80){++in_;in_us_+=elapsed;}else{++out_;out_us_+=elapsed;}
    control_us_+=elapsed;max_control_us_=(std::max)(max_control_us_,elapsed);
    if (!code && actual != size)
        code = ERROR_INVALID_DATA;
    if (!code && read && size)
        std::memcpy(data, request.buffer.data(), size);
    if(code) {
        last_error_=code;failed_=Error(code);
        msg_err("PIX-DT295: USB control type=%02x request=%02x value=%04x index=%04x bytes=%u actual=%lu win32=%lu errno=%d\n",
            type,request_code,value,index,size,actual,code,failed_);
    }
    return failed_;
}
}
