// itedev_bus_winusb.c

#if !defined(_WIN32) && !defined(_WIN64)
#error Unsupported platform.
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <process.h>

#include <windows.h>

#include "itedtv_bus.h"

/* it930x の制御メッセージは送信255バイト、受信256バイト以内に収まる。 */
#define ITEDTV_USB_CTRL_BUFFER_SIZE	256

struct itedtv_usb_context;

/* 中止後に完了が遅れても書き込み先が残るよう、制御転送は context の領域で行う。 */
struct itedtv_usb_ctrl_xfer {
	OVERLAPPED ol;
	uint8_t buffer[ITEDTV_USB_CTRL_BUFFER_SIZE];
};

struct itedtv_usb_work {
	struct itedtv_usb_context *ctx;
	OVERLAPPED ol;
	void *buffer;
	size_t size;
	bool can_submit;
	bool submitted;
};

struct itedtv_usb_context {
	CRITICAL_SECTION lock;
	HANDLE ctrl_event[2];
	struct itedtv_bus *bus;
	itedtv_bus_stream_handler_t stream_handler;
	void *ctx;
	uint32_t num_urb;
	bool no_raw_io;
	uint32_t num_works;
	struct itedtv_usb_work *works;
	LONG streaming;
	HANDLE worker_thread;
	HANDLE startup_event;
	LONG startup_result;
	/* RAW_IO の復元に失敗したパイプは再利用しない。 */
	bool policy_failed;
	bool quarantined;
	LONG faulted;
	struct itedtv_usb_ctrl_xfer ctrl_xfer[2];
	/* 中止後も完了しない単発転送がある。ctx と event はプロセス終了まで保持する。 */
	LONG stranded;
};

static int winerr_to_errno(struct device *dev)
{
	int ret = 0;

	switch (GetLastError()) {
	case ERROR_SUCCESS:
		ret = 0;
		break;

	case ERROR_INVALID_HANDLE:
		ret = EINVAL;
		break;

	case ERROR_IO_PENDING:
	case ERROR_IO_INCOMPLETE:
		ret = ETIMEDOUT;
		break;

	case ERROR_NOT_ENOUGH_MEMORY:
		ret = ENOMEM;
		break;

	case ERROR_SEM_TIMEOUT:
		ret = ETIMEDOUT;
		break;

	case ERROR_BAD_COMMAND:
	case ERROR_GEN_FAILURE:
		ret = EIO;
		break;
	case ERROR_DEVICE_NOT_CONNECTED:
	case ERROR_NO_SUCH_DEVICE:
		ret = ENODEV;
		break;
	case ERROR_OPERATION_ABORTED:
		ret = ECANCELED;
		break;

	default:
		dev_err(dev, "winusb unknown error: 0x%08x\n", GetLastError());
		ret = EINVAL;
		break;
	}

	return ret;
}

/*
 * 中止した要求の完了を ctrl_timeout まで待つ。待機は event で行い、WinUSB の
 * handle の状態に依存しない。完了しない要求は kernel が後から OVERLAPPED と
 * buffer へ書き込むため、false を受けた呼び出し元は両方を解放せず隔離する。
 */
static bool itedtv_usb_settle(struct itedtv_bus *bus, OVERLAPPED *ol, ULONG *rlen)
{
	struct usb_device *dev = bus->usb.dev;

	CancelIoEx(dev->dev, ol);
	if (WaitForSingleObject(ol->hEvent, bus->usb.ctrl_timeout) != WAIT_OBJECT_0)
		return false;
	WinUsb_GetOverlappedResult(dev->winusb, ol, rlen, FALSE);
	return true;
}

static void itedtv_usb_strand(struct itedtv_bus *bus, UCHAR pipe)
{
	struct itedtv_usb_context *ctx = bus->usb.priv;

	InterlockedExchange(&ctx->stranded, 1);
	InterlockedExchange(&ctx->faulted, 1);
	dev_err(bus->dev, "USB transfer on pipe 0x%02x did not complete after cancellation; buffers retained, device restart required\n", pipe);
}

static int itedtv_usb_ctrl_tx(struct itedtv_bus *bus, void *buf, int len)
{
	int ret = 0;
	struct usb_device *dev = bus->usb.dev;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	struct itedtv_usb_ctrl_xfer *xfer = &ctx->ctrl_xfer[0];
	ULONG rlen = 0;
	bool pending = false;

	if (!buf || len <= 0)
		return -EINVAL;
	if (len > ITEDTV_USB_CTRL_BUFFER_SIZE)
		return -EMSGSIZE;
	if (InterlockedCompareExchange(&ctx->faulted, 0, 0)) return -EIO;

	memcpy(xfer->buffer, buf, len);
	memset(&xfer->ol, 0, sizeof(xfer->ol));
	xfer->ol.hEvent = ctx->ctrl_event[0];
	ResetEvent(xfer->ol.hEvent);

	/* Endpoint 0x02: Host->Device bulk endpoint for controlling the device */
	if (!WinUsb_WritePipe(dev->winusb, 0x02, xfer->buffer, len, NULL, &xfer->ol)) {
		if (GetLastError() == ERROR_IO_PENDING) {
			pending = true;
			WaitForSingleObject(xfer->ol.hEvent, bus->usb.ctrl_timeout);
		} else
			ret = -winerr_to_errno(bus->dev);
	} else {
		dev_dbg(bus->dev, "itedtv_usb_ctrl_tx: WinUsb_WritePipe() TRUE\n");
	}

	if (!ret) {
		if (!WinUsb_GetOverlappedResult(dev->winusb, &xfer->ol, &rlen, FALSE))
			ret = -winerr_to_errno(bus->dev);
		else if (rlen != (ULONG)len)
			ret = -EIO;
	}

	if (ret) {
		WinUsb_AbortPipe(dev->winusb, 0x02);
		/* 投入時に失敗した要求は event も OVERLAPPED も更新されず、完了を待つと戻らない。 */
		if (pending && !itedtv_usb_settle(bus, &xfer->ol, &rlen))
			itedtv_usb_strand(bus, 0x02);
	}

	return ret;
}

static int itedtv_usb_ctrl_rx(struct itedtv_bus *bus, void *buf, int *len)
{
	int ret = 0;
	struct usb_device *dev = bus->usb.dev;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	struct itedtv_usb_ctrl_xfer *xfer = &ctx->ctrl_xfer[1];
	ULONG rlen = 0;
	ULONG capacity;
	bool pending = false;

	if (!buf || !len || *len <= 0)
		return -EINVAL;
	if (InterlockedCompareExchange(&ctx->faulted, 0, 0)) return -EIO;

	/* 応答は256バイト以内のため、それ以上の容量は受信長の上限にならない。 */
	capacity = (*len < ITEDTV_USB_CTRL_BUFFER_SIZE) ? (ULONG)*len : ITEDTV_USB_CTRL_BUFFER_SIZE;
	memset(&xfer->ol, 0, sizeof(xfer->ol));
	xfer->ol.hEvent = ctx->ctrl_event[1];
	ResetEvent(xfer->ol.hEvent);

	/* Endpoint 0x81: Device->Host bulk endpoint for controlling the device */
	if (!WinUsb_ReadPipe(dev->winusb, 0x81, xfer->buffer, capacity, NULL, &xfer->ol)) {
		if (GetLastError() == ERROR_IO_PENDING) {
			pending = true;
			WaitForSingleObject(xfer->ol.hEvent, bus->usb.ctrl_timeout);
		} else
			ret = -winerr_to_errno(bus->dev);
	} else {
		dev_dbg(bus->dev, "itedtv_usb_ctrl_rx: WinUsb_ReadPipe() TRUE\n");
	}

	if (!ret) {
		if (!WinUsb_GetOverlappedResult(dev->winusb, &xfer->ol, &rlen, FALSE))
			ret = -winerr_to_errno(bus->dev);
		else if (rlen > capacity)
			ret = -EIO;
	}

	if (ret) {
		WinUsb_AbortPipe(dev->winusb, 0x81);
		if (pending && !itedtv_usb_settle(bus, &xfer->ol, &rlen))
			itedtv_usb_strand(bus, 0x81);
		*len = -1;
	} else {
		memcpy(buf, xfer->buffer, rlen);
		*len = rlen;
	}

	return ret;
}

static int itedtv_usb_stream_rx(struct itedtv_bus *bus, void *buf, int *len, int timeout)
{
	int ret = 0;
	struct usb_device *dev = bus->usb.dev;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	ULONG rlen = 0;
	OVERLAPPED *ol;
	uint8_t *data;
	bool pending = false;

	if (!buf || !len || *len <= 0)
		return -EINVAL;
	if (InterlockedCompareExchange(&ctx->faulted, 0, 0)) return -EIO;

	/* 呼び出し元は返却直後に buffer を解放するため、完了未確認時に隔離できる領域で受ける。 */
	ol = calloc(1, sizeof(*ol) + (size_t)*len);
	if (!ol) {
		*len = 0;
		return -ENOMEM;
	}
	data = (uint8_t *)(ol + 1);

	ol->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!ol->hEvent) {
		free(ol);
		*len = 0;
		return -winerr_to_errno(bus->dev);
	}

	/* Endpoint 0x84: Device->Host bulk endpoint for receiving TS from the device */
	if (!WinUsb_ReadPipe(dev->winusb, 0x84, data, *len, NULL, ol)) {
		if (GetLastError() == ERROR_IO_PENDING) {
			pending = true;
			WaitForSingleObject(ol->hEvent, timeout);
		} else
			ret = -winerr_to_errno(bus->dev);
	} else {
		dev_dbg(bus->dev, "itedtv_usb_stream_rx: WinUsb_ReadPipe() TRUE\n");
	}

	if (!ret) {
		if (!WinUsb_GetOverlappedResult(dev->winusb, ol, &rlen, FALSE))
			ret = -winerr_to_errno(bus->dev);
		else if (rlen > (ULONG)*len)
			ret = -EIO;
	}

	if (ret) {
		WinUsb_AbortPipe(dev->winusb, 0x84);
		*len = -1;
		if (pending && !itedtv_usb_settle(bus, ol, &rlen)) {
			itedtv_usb_strand(bus, 0x84);
			return ret;
		}
	} else {
		memcpy(buf, data, rlen);
		*len = rlen;
	}

	CloseHandle(ol->hEvent);
	free(ol);
	return ret;
}

static int itedtv_usb_alloc_work_buffers(struct itedtv_usb_context *ctx, uint32_t buf_size)
{
	uint32_t i;
	struct itedtv_bus *bus = ctx->bus;
	struct usb_device *dev = bus->usb.dev;
	uint32_t num = ctx->num_works;
	struct itedtv_usb_work *works = ctx->works;

	if (!works)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		void *p;

		works[i].ctx = ctx;

		if (!works[i].ol.hEvent) {
			works[i].ol.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
			if (!works[i].ol.hEvent) {
				dev_err(bus->dev, "itedtv_usb_alloc_work_buffers: CreateEventW() failed. (i: %u)\n", i);
				break;
			}
		}

		if (works[i].buffer && works[i].size != buf_size) {
			VirtualFree(works[i].buffer, 0, MEM_RELEASE);
			works[i].buffer = NULL;
		}

		if (!works[i].buffer) {
			p = VirtualAlloc(NULL, buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (!p) {
				dev_err(bus->dev, "itedtv_usb_alloc_work_buffers: VirtualAlloc() failed. (i: %u)\n", i);
				CloseHandle(works[i].ol.hEvent);
				works[i].ol.hEvent = NULL;
				break;
			}

			dev_dbg(bus->dev, "itedtv_usb_alloc_work_buffers: p: %p, buf_size: %u\n", p, buf_size);

			works[i].buffer = p;
			works[i].size = buf_size;
		}

		works[i].can_submit = true;
	}

	ctx->num_urb = i;

	if (!i)
		return -ENOMEM;

	return 0;
}

static void itedtv_usb_free_work_buffers(struct itedtv_usb_context *ctx)
{
	uint32_t i;
	struct usb_device *dev = ctx->bus->usb.dev;
	uint32_t num = ctx->num_works;
	struct itedtv_usb_work *works = ctx->works;

	if (!works)
		return;

	for (i = 0; i < num; i++) {
		if (works[i].ol.hEvent) {
			CloseHandle(works[i].ol.hEvent);
			works[i].ol.hEvent = NULL;
		}

		if (works[i].buffer) {
			VirtualFree(works[i].buffer, 0, MEM_RELEASE);
			works[i].buffer = NULL;
		}

		works[i].size = 0;
		works[i].can_submit = false;
	}

	ctx->num_urb = 0;

	return;
}

static void itedtv_usb_clean_context(struct itedtv_usb_context *ctx)
{
	/* 完了未確認の IRP が参照する領域は、プロセス終了まで隔離する。 */
	if (ctx->quarantined) return;
	if (ctx->works) {
		itedtv_usb_free_work_buffers(ctx);
		free(ctx->works);
	}

	ctx->stream_handler = NULL;
	ctx->ctx = NULL;
	ctx->num_urb = 0;
	ctx->num_works = 0;
	ctx->works = NULL;
}

static void itedtv_usb_drain(struct itedtv_usb_context *ctx)
{
	/* 投入済み要求は成功/中止の完了を回収するまで buffer と event を保持する。 */
	WinUsb_AbortPipe(ctx->bus->usb.dev->winusb, 0x84);
	const ULONGLONG deadline = GetTickCount64() + ctx->bus->usb.ctrl_timeout;
	for (uint32_t i = 0; i < ctx->num_works; ++i) {
		struct itedtv_usb_work *work = &ctx->works[i];
		if (work->submitted) {
			ULONG received = 0;
			CancelIoEx(ctx->bus->usb.dev->dev, &work->ol);
			const ULONGLONG now = GetTickCount64();
			WaitForSingleObject(work->ol.hEvent, now < deadline ? (DWORD)(deadline - now) : 0);
			if (!WinUsb_GetOverlappedResult(ctx->bus->usb.dev->winusb, &work->ol, &received, FALSE)) {
				DWORD error = GetLastError();
				if (error == ERROR_IO_INCOMPLETE || error == ERROR_INVALID_HANDLE) {
					ctx->quarantined = true;
					InterlockedExchange(&ctx->faulted, 1);
					continue;
				}
			}
			work->can_submit = true;
			work->submitted = false;
		}
	}
	if (ctx->quarantined)
		dev_err(ctx->bus->dev, "USB cancellation incomplete; buffers retained, device restart required\n");
}

unsigned __stdcall itedtv_winusb_worker(void *arg)
{
	struct itedtv_usb_context *ctx = arg;
	struct itedtv_bus *bus = ctx->bus;
	uint32_t i, num, next_idx = 0;
	struct itedtv_usb_work *works;
	WINUSB_INTERFACE_HANDLE winusb;
	UCHAR raw_io;
	int startup_result = 0;

	dev_dbg(bus->dev, "itedtv_winusb_worker: start\n");

	num = ctx->num_urb;
	works = ctx->works;
	winusb = bus->usb.dev->winusb;
	raw_io = (ctx->no_raw_io) ? 0 : 1;

	if (!WinUsb_SetPipePolicy(winusb, 0x84, RAW_IO, sizeof(raw_io), &raw_io)) {
		startup_result = -winerr_to_errno(bus->dev);
		dev_err(bus->dev, "itedtv_winusb_worker: WinUsb_SetPipePolicy(RAW_IO, %u) failed.\n", raw_io);
		goto startup_failed;
	}

	for (i = 0; i < num; i++) {
		if (!works[i].can_submit)
			continue;

		works[i].can_submit = false;
		works[i].submitted = true;
		ResetEvent(works[i].ol.hEvent);

		if (WinUsb_ReadPipe(winusb, 0x84, works[i].buffer, (ULONG)works[i].size, NULL, &works[i].ol))
			continue;

		if (GetLastError() == ERROR_IO_PENDING)
			continue;

		startup_result = -winerr_to_errno(bus->dev);
		dev_err(bus->dev, "itedtv_winusb_worker: WinUsb_ReadPipe() 1 failed. (%u, %u)\n", i, GetLastError());
		works[i].can_submit = true;
		works[i].submitted = false;

		num = i;
		break;
	}

	dev_dbg(bus->dev, "itedtv_winusb_worker: num: %u\n", num);
	if (!num)
		goto startup_failed;

	InterlockedExchange(&ctx->startup_result, 0);
	SetEvent(ctx->startup_event);

	while (ctx->streaming) {
		uint32_t idx = next_idx;
		DWORD ret, rlen = 0;
		struct itedtv_usb_work *work = &works[idx];

		if (work->can_submit) {
			dev_dbg(bus->dev, "itedtv_winusb_worker: can_submit %u\n", idx);
			for (i = (idx + 1) % num; i != idx; i = (i + 1) % num) {
				if (!works[i].can_submit)
					break;
			}
			if (i == idx)
				break;

			idx = i;
			work = &works[idx];
		}

		ret = WaitForSingleObject(work->ol.hEvent, 500);
		if (ret == WAIT_TIMEOUT) {
			dev_dbg(bus->dev, "itedtv_winusb_worker: timeout %u\n", idx);
			continue;
		} else if (ret == WAIT_FAILED)
			break;

		if (WinUsb_GetOverlappedResult(winusb, &work->ol, &rlen, TRUE))
			ctx->stream_handler(ctx->ctx, work->buffer, rlen);
		work->submitted = false;

		next_idx = (idx + 1) % num;
		ResetEvent(work->ol.hEvent);
		work->submitted = true;

		if (WinUsb_ReadPipe(winusb, 0x84, work->buffer, (ULONG)work->size, NULL, &work->ol))
			continue;

		if (GetLastError() == ERROR_IO_PENDING)
			continue;

		dev_err(bus->dev, "itedtv_winusb_worker: WinUsb_ReadPipe() 2 failed. (%u, %u)\n", idx, GetLastError());
		work->can_submit = true;
		work->submitted = false;
	}

	goto exit;

startup_failed:
	InterlockedExchange(&ctx->streaming, 0);
	InterlockedExchange(&ctx->startup_result, startup_result ? startup_result : -EIO);
	SetEvent(ctx->startup_event);

exit:
	itedtv_usb_drain(ctx);
	if (raw_io && !ctx->quarantined) {
		raw_io = 0;
		if (!WinUsb_SetPipePolicy(winusb, 0x84, RAW_IO, sizeof(raw_io), &raw_io)) {
			ctx->policy_failed = true;
			InterlockedExchange(&ctx->faulted, 1);
			dev_err(bus->dev, "RAW_IO restore failed: error=%lu; reopen device required\n", GetLastError());
		}
	}
	InterlockedExchange(&ctx->streaming, 0);
	dev_dbg(bus->dev, "itedtv_winusb_worker: exit\n");
	return 0;
}

static int itedtv_usb_start_streaming(struct itedtv_bus *bus, itedtv_bus_stream_handler_t stream_handler, void *context)
{
	int ret = 0;
	u32 buf_size, num;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	struct itedtv_usb_work *works;

	if (!stream_handler)
		return -EINVAL;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming\n");

	EnterCriticalSection(&ctx->lock);
	if (ctx->worker_thread || ctx->policy_failed || ctx->quarantined) {
		LeaveCriticalSection(&ctx->lock);
		return ctx->policy_failed || ctx->quarantined ? -EIO : -EBUSY;
	}

	ctx->stream_handler = stream_handler;
	ctx->ctx = context;
	ctx->no_raw_io = bus->usb.streaming.no_raw_io;

	buf_size = bus->usb.streaming.urb_buffer_size;
	num = bus->usb.streaming.urb_num;

	if (!buf_size || !num || num > 64) {
		ret = -EINVAL;
		goto fail;
	}

	if (!ctx->no_raw_io) {
		ULONG maximum = 0, length = sizeof(maximum);
		const u32 packet = bus->usb.max_bulk_size;
		if (!packet || !WinUsb_GetPipePolicy(bus->usb.dev->winusb, 0x84,
			MAXIMUM_TRANSFER_SIZE, &length, &maximum)) {
			ret = packet ? -winerr_to_errno(bus->dev) : -EINVAL;
			goto fail;
		}
		/* 加算前に上限との差を検査し、32bit の丸め溢れも拒否する。 */
		const u32 padding = (packet - buf_size % packet) % packet;
		if (length != sizeof(maximum) || buf_size > maximum || padding > maximum - buf_size) {
			dev_err(bus->dev, "RAW_IO transfer exceeds limit: requested=%u packet=%u maximum=%lu\n", buf_size, packet, maximum);
			ret = -EMSGSIZE;
			goto fail;
		}
		buf_size += padding;
	}

	if (ctx->works && num != ctx->num_works) {
		itedtv_usb_free_work_buffers(ctx);
		free(ctx->works);
		ctx->works = NULL;
	}

	ctx->num_works = num;

	if (!ctx->works) {
		ctx->works = calloc(ctx->num_works, sizeof(*works));
		if (!ctx->works) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	ret = itedtv_usb_alloc_work_buffers(ctx, buf_size);
	if (ret)
		goto fail;

	ctx->startup_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!ctx->startup_event) {
		ret = -winerr_to_errno(bus->dev);
		goto fail;
	}
	InterlockedExchange(&ctx->startup_result, -EIO);

	if (!WinUsb_ResetPipe(bus->usb.dev->winusb, 0x84)) {
		ret = -winerr_to_errno(bus->dev);
		goto fail;
	}
	InterlockedExchange(&ctx->streaming, 1);

	ctx->worker_thread = (HANDLE)_beginthreadex(NULL, 0, itedtv_winusb_worker, ctx, 0, NULL);
	if (!ctx->worker_thread) {
		ret = -EAGAIN;
		dev_err(bus->dev, "itedtv_usb_start_streaming: _beginthreadex() failed.\n");
		goto fail;
	}

	if (!SetThreadPriority(ctx->worker_thread, THREAD_PRIORITY_TIME_CRITICAL))
		dev_dbg(bus->dev, "itedtv_usb_start_streaming: SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL) failed.\n");

	/* 最初の USB 転送を投入できたことを確認してから受信開始を成功として返す */
	{
		DWORD wait_result = WaitForSingleObject(ctx->startup_event, bus->usb.ctrl_timeout);
		if (wait_result == WAIT_OBJECT_0)
			ret = InterlockedCompareExchange(&ctx->startup_result, 0, 0);
		else if (wait_result == WAIT_TIMEOUT)
			ret = -ETIMEDOUT;
		else
			ret = -EIO;
	}
	if (ret)
		goto fail;

	CloseHandle(ctx->startup_event);
	ctx->startup_event = NULL;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming: num: %u\n", num);

	LeaveCriticalSection(&ctx->lock);

	return ret;

fail:
	InterlockedExchange(&ctx->streaming, 0);

	if (ctx->worker_thread) {
		WaitForSingleObject(ctx->worker_thread, INFINITE);
		CloseHandle(ctx->worker_thread);
		ctx->worker_thread = NULL;
	}

	if (ctx->startup_event) {
		CloseHandle(ctx->startup_event);
		ctx->startup_event = NULL;
	}

	itedtv_usb_clean_context(ctx);

	LeaveCriticalSection(&ctx->lock);

	return ret;
}

static int itedtv_usb_stop_streaming(struct itedtv_bus *bus)
{
	struct itedtv_usb_context *ctx = bus->usb.priv;

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming\n");

	EnterCriticalSection(&ctx->lock);

	InterlockedExchange(&ctx->streaming, 0);

	if (ctx->worker_thread) {
		/* Abort は起床の補助。完了回収は worker が所有する。 */
		WinUsb_AbortPipe(bus->usb.dev->winusb, 0x84);
		WaitForSingleObject(ctx->worker_thread, INFINITE);
		CloseHandle(ctx->worker_thread);
		ctx->worker_thread = NULL;
	}

	itedtv_usb_clean_context(ctx);

	LeaveCriticalSection(&ctx->lock);

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming: exit\n");
	return ctx->quarantined || ctx->policy_failed ? -EIO : 0;
}

int itedtv_bus_init(struct itedtv_bus *bus)
{
	int ret = 0;

	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		UCHAR raw_io;
		struct itedtv_usb_context *ctx;

		if (!bus->usb.dev) {
			ret = -EINVAL;
			break;
		}

		if (bus->usb.dev->descriptor.bcdUSB < 0x0110) {
			ret = -EIO;
			break;
		}

		/* 実際の endpoint の USB packet size を使い、TS の188byteと区別する。 */
		USB_INTERFACE_DESCRIPTOR interface_descriptor;
		WINUSB_PIPE_INFORMATION pipe;
		bool found = false;
		if (!WinUsb_QueryInterfaceSettings(bus->usb.dev->winusb, 0, &interface_descriptor)) {
			ret = -winerr_to_errno(bus->dev);
			break;
		}
		for (UCHAR i = 0; i < interface_descriptor.bNumEndpoints; ++i) {
			if (!WinUsb_QueryPipe(bus->usb.dev->winusb, 0, i, &pipe)) {
				ret = -winerr_to_errno(bus->dev);
				break;
			}
			if (pipe.PipeId == 0x84 && pipe.PipeType == UsbdPipeTypeBulk && pipe.MaximumPacketSize) {
				bus->usb.max_bulk_size = pipe.MaximumPacketSize;
				found = true;
			}
		}
		if (ret || !found) {
			if (!ret) ret = -ENODEV;
			break;
		}

		raw_io = 1;
		if (!WinUsb_SetPipePolicy(bus->usb.dev->winusb, 0x84, RAW_IO, sizeof(raw_io), &raw_io)) {
			ret = -winerr_to_errno(bus->dev);
			break;
		}

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			ret = -ENOMEM;
			break;
		}

		InitializeCriticalSection(&ctx->lock);
		for (int i = 0; i < 2; i++) {
			ctx->ctrl_event[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
			if (!ctx->ctrl_event[i]) {
				ret = -winerr_to_errno(bus->dev);
				break;
			}
		}
		if (ret) {
			for (int i = 0; i < 2; ++i)
				if (ctx->ctrl_event[i]) CloseHandle(ctx->ctrl_event[i]);
			DeleteCriticalSection(&ctx->lock);
			free(ctx);
			break;
		}
		ctx->bus = bus;
		ctx->stream_handler = NULL;
		ctx->ctx = NULL;
		ctx->num_urb = 0;
		ctx->num_works = 0;
		ctx->works = NULL;
		ctx->streaming = 0;
		ctx->worker_thread = NULL;
		ctx->startup_event = NULL;
		ctx->startup_result = 0;

		bus->usb.priv = ctx;

		if (!bus->usb.max_bulk_size)
			bus->usb.max_bulk_size = (bus->usb.dev->descriptor.bcdUSB == 0x0110) ? 64 : 512;

		bus->ops.ctrl_tx = itedtv_usb_ctrl_tx;
		bus->ops.ctrl_rx = itedtv_usb_ctrl_rx;
		bus->ops.stream_rx = itedtv_usb_stream_rx;
		bus->ops.start_streaming = itedtv_usb_start_streaming;
		bus->ops.stop_streaming = itedtv_usb_stop_streaming;

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int itedtv_bus_term(struct itedtv_bus *bus)
{
	int ret = 0;
	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		struct itedtv_usb_context *ctx = bus->usb.priv;

		if (ctx) {
			itedtv_usb_stop_streaming(bus);
			if (ctx->quarantined || InterlockedCompareExchange(&ctx->stranded, 0, 0)) {
				/* DeviceBase はUSB handleを閉じるが、遅延完了先の領域は解放しない。 */
				ret = -EIO;
				break;
			}
			CloseHandle(ctx->ctrl_event[0]);
			CloseHandle(ctx->ctrl_event[1]);
			DeleteCriticalSection(&ctx->lock);
			free(ctx);
		}

		break;
	}

	default:
		break;
	}

	dev_dbg(bus->dev, "itedtv_bus_term: exit\n");

	memset(bus, 0, sizeof(*bus));

	return ret;
}
