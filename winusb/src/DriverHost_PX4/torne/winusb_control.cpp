// SPDX-License-Identifier: GPL-2.0-only
#include "winusb_control.hpp"

#include <algorithm>
#include <cstring>
#include <process.h>

namespace px4::torne {

namespace {
// MSi2500 の停止処理と同じ回収期限。転送が完了しない場合は別に回収する。
constexpr DWORD CancelCompletionTimeoutMs = 1000;
}

DWORD NativeControlIo::Submit(WINUSB_INTERFACE_HANDLE usb, const MemoryRequest &request,
	std::uint8_t *data, USHORT length, OVERLAPPED &operation) noexcept
{
	WINUSB_SETUP_PACKET setup{};
	setup.RequestType = request.type;
	setup.Request = request.request;
	setup.Value = request.value;
	setup.Index = request.index;
	setup.Length = length;
	return WinUsb_ControlTransfer(usb, setup, data, length, nullptr, &operation) ? ERROR_SUCCESS : GetLastError();
}

DWORD NativeControlIo::Collect(WINUSB_INTERFACE_HANDLE usb, OVERLAPPED &operation,
	ULONG &transferred, bool wait) noexcept
{
	return WinUsb_GetOverlappedResult(usb, &operation, &transferred, wait ? TRUE : FALSE) ? ERROR_SUCCESS : GetLastError();
}

DWORD NativeControlIo::Cancel(HANDLE file, OVERLAPPED &operation) noexcept
{
	// AbortPipeでは別要求まで取り消されるため、当該OVERLAPPEDだけを対象にする。
	return CancelIoEx(file, &operation) ? ERROR_SUCCESS : GetLastError();
}

WinUsbHandles::~WinUsbHandles()
{
    if (owned_) {
        if (usb_) WinUsb_Free(usb_);
        if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
}

WinUsbRequest::WinUsbRequest(std::size_t size) : buffer(size)
{
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WinUsbRequest::~WinUsbRequest()
{
    if (operation.hEvent) CloseHandle(operation.hEvent);
}

bool WinUsbRequest::Reset() noexcept
{
    const HANDLE event = operation.hEvent;
    operation = {};
    operation.hEvent = event;
    return ResetEvent(event) != FALSE;
}

unsigned __stdcall WinUsbRequest::Reap(void *context) noexcept
{
    auto *request = static_cast<WinUsbRequest *>(context);
    if (WaitForSingleObject(request->operation.hEvent, INFINITE) == WAIT_OBJECT_0) {
        --request->retained_handles_->deferred_;
        delete request;
    }
    return 0;
}

DWORD WinUsbRequest::Finish(std::unique_ptr<WinUsbRequest> &request,
    const std::shared_ptr<WinUsbHandles> &handles, ControlIo &io, DWORD submitted, DWORD reason,
    ULONG &transferred) noexcept
{
    auto &operation = request->operation;
    if (reason) {
        io.Cancel(handles->File(), operation);
        // 通常の取り消しはここで回収する。応答がない場合も呼び出し側を待たせ続けない。
        if (submitted == ERROR_IO_PENDING)
            WaitForSingleObject(operation.hEvent, CancelCompletionTimeoutMs);
    }

    const DWORD result = io.Collect(handles->Usb(), operation, transferred, false);
    if (submitted == ERROR_IO_PENDING && result != ERROR_SUCCESS &&
        WaitForSingleObject(operation.hEvent, 0) != WAIT_OBJECT_0) {
        if (!reason)
            io.Cancel(handles->File(), operation);
        // 完了が確認できるまで同じ個体への新規要求を止め、USB と要求の資源を保持する。
        request->retained_handles_ = handles;
        ++handles->deferred_;
        auto *pending = request.release();
        const auto thread = _beginthreadex(nullptr, 0, Reap, pending, 0, nullptr);
        if (thread)
            CloseHandle(reinterpret_cast<HANDLE>(thread));
        else
            OutputDebugStringW(L"torne: cannot start USB completion waiter; retaining pending request\n");
    }
    return reason ? reason : result;
}

WinUsbControl::WinUsbControl(std::shared_ptr<WinUsbHandles> handles, HANDLE cancel, ControlIo &io) noexcept
    : WinUsbControl(handles->File(), handles->Usb(), cancel, io)
{
    handles_ = std::move(handles);
}

WinUsbControl::WinUsbControl(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE cancel, ControlIo &io) noexcept
    : file_(file), usb_(usb), cancel_(cancel), io_(io)
{}

int WinUsbControl::Pause(unsigned milliseconds)
{
	if(auto error=Cancellation())return static_cast<int>(error);
	const auto now = Clock::now();
	if (now >= deadline_)
		return ERROR_TIMEOUT;
	const auto end = (std::min)(deadline_, now + std::chrono::milliseconds(milliseconds));
	const auto timeout = std::chrono::ceil<std::chrono::milliseconds>(end - now).count();
	const HANDLE events[]{cancel_,stop_};
	const DWORD result = WaitForMultipleObjects(stop_?2:1,events,FALSE,static_cast<DWORD>((std::min)(timeout,static_cast<decltype(timeout)>(MAXDWORD-1))));
	if(result==WAIT_OBJECT_0 || (stop_ && result==WAIT_OBJECT_0+1))
		return ERROR_OPERATION_ABORTED;
	if (result == WAIT_FAILED)
		return static_cast<int>(GetLastError());
	if (result != WAIT_TIMEOUT)
		return ERROR_INVALID_DATA;
	return Clock::now() >= deadline_ ? ERROR_TIMEOUT : ERROR_SUCCESS;
}

int WinUsbControl::Transfer(const MemoryRequest &request, std::uint8_t *data,
	std::size_t length, std::size_t &transferred)
{
	if (!data || !length || length > 64) {
		transferred = 0;
		return ERROR_INVALID_PARAMETER;
	}
	return Execute(data, length, (request.type & 0x80) != 0, transferred, deadline_, [&](std::uint8_t *buffer, OVERLAPPED &operation) {
        return io_.Submit(usb_, request, buffer, static_cast<USHORT>(length), operation);
	});
}

int WinUsbControl::TransferPipe(UCHAR endpoint, std::uint8_t *data, std::size_t length,
	std::size_t &transferred, Clock::time_point deadline)
{
	if (!data || !length || length > 512) {
		transferred = 0;
		return ERROR_INVALID_PARAMETER;
	}
	return Execute(data, length, (endpoint & 0x80) != 0, transferred, (std::min)(deadline, deadline_),
        [&](std::uint8_t *buffer, OVERLAPPED &operation) {
        const auto submit = endpoint & 0x80 ? WinUsb_ReadPipe : WinUsb_WritePipe;
        return submit(usb_, endpoint, buffer, static_cast<ULONG>(length), nullptr, &operation) ?
			ERROR_SUCCESS : GetLastError();
	});
}

template<class Submit>
int WinUsbControl::Execute(std::uint8_t *data, std::size_t length, bool read, std::size_t &transferred,
    Clock::time_point deadline, Submit submit)
{
	transferred = 0;
	if (file_ == INVALID_HANDLE_VALUE || !file_ || !usb_ || !cancel_)
		return ERROR_INVALID_PARAMETER;
	if(auto error=Cancellation())return static_cast<int>(error);
	if (Clock::now() >= deadline)
		return ERROR_TIMEOUT;

    if (handles_ && !handles_->Ready())
        return ERROR_BUSY;

    // 共有所有権がある場合だけ、完了を待たずに呼び出し側へ返せるバッファを用意する。
    std::unique_ptr<WinUsbRequest> pending;
    std::unique_ptr<void, decltype(&CloseHandle)> local_event(nullptr, &CloseHandle);
    OVERLAPPED local_operation{};
    OVERLAPPED *operation = &local_operation;
    auto *buffer = data;
    if (handles_) {
        try {
            pending = std::make_unique<WinUsbRequest>(length);
        } catch (const std::bad_alloc &) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        operation = &pending->operation;
        buffer = pending->buffer.data();
        if (!read)
            std::memcpy(buffer, data, length);
    } else {
        local_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        operation->hEvent = local_event.get();
    }
    const HANDLE completion = operation->hEvent;
    if (!completion)
        return static_cast<int>(GetLastError());
    const DWORD submitted = submit(buffer, *operation);
    if (submitted != ERROR_SUCCESS && submitted != ERROR_IO_PENDING)
        return static_cast<int>(submitted);

	DWORD reason = ERROR_SUCCESS;
	if (Clock::now() >= deadline) {
		reason = ERROR_TIMEOUT;
	} else if (submitted == ERROR_IO_PENDING) {
		const auto remaining = deadline - Clock::now();
		// 切り捨てで期限より早く切らないよう、残時間をミリ秒へ切り上げる。
		const auto millis = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
		const auto timeout = static_cast<DWORD>(std::clamp<decltype(millis)>(millis, 0, MAXDWORD - 1));
		const HANDLE events[] = { cancel_, completion, stop_ };
		const DWORD result = WaitForMultipleObjects(stop_?3:2, events, FALSE, timeout);
		if (result == WAIT_OBJECT_0 || (stop_ && result==WAIT_OBJECT_0+2))
			reason = ERROR_OPERATION_ABORTED;
		else if (result == WAIT_TIMEOUT)
			reason = ERROR_TIMEOUT;
		else if (result != WAIT_OBJECT_0 + 1)
			reason = result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
	}
	if (!reason) {
		// 完了とcancelが競合した場合も、停止後の結果を新しい選局へ渡さない。
		reason=Cancellation();
        if(!reason && Clock::now()>=deadline)reason=ERROR_TIMEOUT;
	}
    ULONG actual = 0;
    DWORD result = ERROR_SUCCESS;
    if (pending) {
        result = WinUsbRequest::Finish(pending, handles_, io_, submitted, reason, actual);
    } else {
        // 借りたバッファとイベントは、カーネルが使い終わるまで返却できない。
        if (reason)
            io_.Cancel(file_, *operation);
        const DWORD collected = io_.Collect(usb_, *operation, actual, true);
        result = reason ? reason : collected;
    }
    if (result)
        return static_cast<int>(result);
    if (actual > length)
        return ERROR_INVALID_DATA;
    if (read && pending)
        std::memcpy(data, pending->buffer.data(), actual);
	transferred = actual;
	return ERROR_SUCCESS;
}

DWORD WinUsbControl::Cancellation() const noexcept
{
    if(!cancel_)return ERROR_INVALID_HANDLE;
    const HANDLE events[]{cancel_,stop_};
    const auto result=WaitForMultipleObjects(stop_?2:1,events,FALSE,0);
    return result==WAIT_TIMEOUT?ERROR_SUCCESS:result==WAIT_FAILED?GetLastError():ERROR_OPERATION_ABORTED;
}
} // namespace px4::torne
