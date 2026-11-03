/* 実際の IT930x bus 実装へ模擬 WinUSB を注入し、実機なしで寿命と上限を検証する。 */
#include <windows.h>
#include <winusb.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>

static LONG outstanding, submissions, aborts, unsubmitted_waits;
static ULONG mock_maximum = 4096;
static USHORT mock_packet = 512;
static bool fail_restore, fail_submission, short_write, stuck_transfer;
static LONG fail_submission_at;
static BOOL WINAPI mock_read(WINUSB_INTERFACE_HANDLE h, UCHAR ep, PUCHAR b, ULONG n, PULONG r, LPOVERLAPPED ol)
{
    (void)h; (void)b; (void)n; (void)r;
    /* kernel32 は投入前に Internal を STATUS_PENDING にし、即時失敗では event も完了も通知しない。 */
    if (fail_submission || (fail_submission_at && submissions >= fail_submission_at)) { ol->Internal = STATUS_PENDING; SetLastError(ERROR_BAD_COMMAND); return FALSE; }
    ol->Internal = 1;
    ol->InternalHigh = n;
    ol->Offset = ep;
    InterlockedIncrement(&outstanding);
    InterlockedIncrement(&submissions);
    if (ep != 0x81 && !stuck_transfer) SetEvent(ol->hEvent);
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}
static BOOL WINAPI mock_result(WINUSB_INTERFACE_HANDLE h, LPOVERLAPPED ol, PULONG n, BOOL wait)
{
    (void)h;
    if (ol->Internal == STATUS_PENDING) {
        /* 実機では完了が届かず待機が戻らないため、待機した事実だけを記録する。 */
        if (wait) InterlockedIncrement(&unsubmitted_waits);
        SetLastError(ERROR_IO_INCOMPLETE); return FALSE;
    }
    if (ol->Internal == 1 && !wait && WaitForSingleObject(ol->hEvent, 0) == WAIT_TIMEOUT) {
        SetLastError(ERROR_IO_INCOMPLETE); return FALSE;
    }
    if (ol->Internal == 1) { ol->Internal = 0; InterlockedDecrement(&outstanding); }
    *n = short_write && ol->Offset == 0x02 ? 0 : (ULONG)ol->InternalHigh;
    return TRUE;
}
static BOOL WINAPI mock_abort(WINUSB_INTERFACE_HANDLE h, UCHAR ep)
{ (void)h; (void)ep; InterlockedIncrement(&aborts); return TRUE; }
static BOOL WINAPI mock_cancel(HANDLE h, LPOVERLAPPED ol)
{ (void)h; if (!stuck_transfer) SetEvent(ol->hEvent); return TRUE; }
static BOOL WINAPI mock_reset(WINUSB_INTERFACE_HANDLE h, UCHAR ep)
{ (void)h; (void)ep; return TRUE; }
static BOOL WINAPI mock_set(WINUSB_INTERFACE_HANDLE h, UCHAR ep, ULONG policy, ULONG n, PVOID value)
{
    (void)h; (void)ep; (void)n;
    if (policy == RAW_IO && !*(UCHAR *)value && fail_restore) { SetLastError(ERROR_BAD_COMMAND); return FALSE; }
    return TRUE;
}
static BOOL WINAPI mock_get(WINUSB_INTERFACE_HANDLE h, UCHAR ep, ULONG policy, PULONG n, PVOID value)
{ (void)h; (void)ep; (void)policy; *n = sizeof(mock_maximum); *(ULONG *)value = mock_maximum; return TRUE; }
static BOOL WINAPI mock_interface(WINUSB_INTERFACE_HANDLE h, UCHAR alt, PUSB_INTERFACE_DESCRIPTOR info)
{ (void)h; (void)alt; ZeroMemory(info, sizeof(*info)); info->bNumEndpoints = 1; return TRUE; }
static BOOL WINAPI mock_pipe(WINUSB_INTERFACE_HANDLE h, UCHAR alt, UCHAR index, PWINUSB_PIPE_INFORMATION info)
{ (void)h; (void)alt; (void)index; ZeroMemory(info, sizeof(*info)); info->PipeId = 0x84; info->PipeType = UsbdPipeTypeBulk; info->MaximumPacketSize = mock_packet; return TRUE; }

#define WinUsb_ReadPipe mock_read
#define WinUsb_WritePipe mock_read
#define WinUsb_GetOverlappedResult mock_result
#define WinUsb_AbortPipe mock_abort
#define WinUsb_ResetPipe mock_reset
#define WinUsb_SetPipePolicy mock_set
#define WinUsb_GetPipePolicy mock_get
#define WinUsb_QueryInterfaceSettings mock_interface
#define WinUsb_QueryPipe mock_pipe
#define CancelIoEx mock_cancel
#include "../src/DriverHost_PX4/itedtv_bus_winusb.c"

int msg_printf(const char *format, ...) { (void)format; return 0; }
static int checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++checks; } } while (0)
static int stream(void *context, void *data, uint32_t n)
{
    struct itedtv_usb_context *ctx = context;
    (void)data; (void)n;
    InterlockedExchange(&ctx->streaming, 0);
    return 0;
}
int main(void)
{
    struct device log = {0};
    struct usb_device usb = {0};
    struct itedtv_bus bus = {0};
    usb.descriptor.bcdUSB = 0x0200;
    bus.type = ITEDTV_BUS_USB; bus.dev = &log; bus.usb.dev = &usb;
    bus.usb.ctrl_timeout = 10; bus.usb.streaming.urb_num = 4;
    CHECK(itedtv_bus_init(&bus) == 0);
    CHECK(bus.usb.max_bulk_size == 512);
    struct itedtv_usb_context *ctx = bus.usb.priv;
    bus.usb.streaming.urb_buffer_size = 4097;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == -EMSGSIZE);
    CHECK(submissions == 0);
    bus.usb.streaming.urb_buffer_size = UINT32_MAX;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == -EMSGSIZE);
    bus.usb.streaming.urb_buffer_size = 513;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == 0);
    CHECK(ctx->works[0].size == 1024);
    CHECK(itedtv_usb_stop_streaming(&bus) == 0);
    CHECK(outstanding == 0 && aborts > 0);
    CHECK(itedtv_usb_stop_streaming(&bus) == 0);
    fail_submission = true;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) != 0);
    CHECK(outstanding == 0);
    fail_submission = false;
    fail_submission_at = submissions + 2;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == 0);
    CHECK(itedtv_usb_stop_streaming(&bus) == 0 && outstanding == 0);
    fail_submission_at = 0;
    unsigned char response[8]; int len = sizeof(response);
    CHECK(itedtv_usb_ctrl_rx(&bus, response, &len) == -ETIMEDOUT);
    CHECK(outstanding == 0);
    len = -1;
    CHECK(itedtv_usb_ctrl_rx(&bus, response, &len) == -EINVAL);
    CHECK(itedtv_usb_ctrl_tx(&bus, response, -1) == -EINVAL);
    short_write = true;
    CHECK(itedtv_usb_ctrl_tx(&bus, response, sizeof(response)) == -EIO && outstanding == 0);
    short_write = false;
    /* 投入時の即時失敗は完了待ちへ入らず、エラーをそのまま返す。 */
    fail_submission = true;
    CHECK(itedtv_usb_ctrl_tx(&bus, response, sizeof(response)) == -EIO);
    len = sizeof(response);
    CHECK(itedtv_usb_ctrl_rx(&bus, response, &len) == -EIO && len == -1);
    len = sizeof(response);
    CHECK(itedtv_usb_stream_rx(&bus, response, &len, 10) == -EIO && len == -1);
    CHECK(unsubmitted_waits == 0 && outstanding == 0);
    fail_submission = false;
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    CHECK(winerr_to_errno(&log) == ENODEV);
    fail_restore = true;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == 0);
    CHECK(itedtv_usb_stop_streaming(&bus) == -EIO);
    CHECK(outstanding == 0 && ctx->policy_failed);
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == -EIO);
    CHECK(itedtv_bus_term(&bus) == 0);
    /* 中止しても完了しない模擬IRPは、再利用・解放せず有限時間で隔離する。 */
    fail_restore = false; stuck_transfer = true;
    bus.type = ITEDTV_BUS_USB; bus.dev = &log; bus.usb.dev = &usb;
    bus.usb.ctrl_timeout = 10; bus.usb.streaming.urb_num = 2;
    bus.usb.streaming.urb_buffer_size = 512;
    CHECK(itedtv_bus_init(&bus) == 0);
    ctx = bus.usb.priv;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == 0);
    ULONGLONG before_stop = GetTickCount64();
    CHECK(itedtv_usb_stop_streaming(&bus) == -EIO && ctx->quarantined);
    CHECK(GetTickCount64() - before_stop < 2000);
    CHECK(ctx->works != NULL && outstanding == 2);
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == -EIO);
    void *late_completion_buffer = ctx->works[0].buffer;
    CHECK(itedtv_bus_term(&bus) == -EIO);
    /* 遅延したkernel書き込み先がterm後も有効である。領域は意図的に終了まで保持する。 */
    memset(late_completion_buffer, 0, 512);
    printf("winusb_lifetime_test: %s\n", checks ? "FAILED" : "PASS");
    return checks ? 1 : 0;
}
