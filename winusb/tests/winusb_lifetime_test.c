/* 実際の IT930x bus 実装へ模擬 WinUSB を注入し、実機なしで寿命と上限を検証する。 */
#include <windows.h>
#include <winusb.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>

static LONG outstanding, submissions, aborts;
static ULONG maximum = 4096;
static USHORT packet = 512;
static bool fail_restore, fail_submission;
static BOOL WINAPI mock_read(WINUSB_INTERFACE_HANDLE h, UCHAR ep, PUCHAR b, ULONG n, PULONG r, LPOVERLAPPED ol)
{
    (void)h; (void)b; (void)n; (void)r;
    if (fail_submission) { SetLastError(ERROR_BAD_COMMAND); return FALSE; }
    ol->Internal = 1;
    InterlockedIncrement(&outstanding);
    InterlockedIncrement(&submissions);
    if (ep == 0x84) SetEvent(ol->hEvent);
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}
static BOOL WINAPI mock_result(WINUSB_INTERFACE_HANDLE h, LPOVERLAPPED ol, PULONG n, BOOL wait)
{
    (void)h;
    if (ol->Internal == 1 && !wait && WaitForSingleObject(ol->hEvent, 0) == WAIT_TIMEOUT) {
        SetLastError(ERROR_IO_INCOMPLETE); return FALSE;
    }
    if (ol->Internal == 1) { ol->Internal = 0; InterlockedDecrement(&outstanding); }
    *n = 188;
    return TRUE;
}
static BOOL WINAPI mock_abort(WINUSB_INTERFACE_HANDLE h, UCHAR ep)
{ (void)h; (void)ep; InterlockedIncrement(&aborts); return TRUE; }
static BOOL WINAPI mock_cancel(HANDLE h, LPOVERLAPPED ol)
{ (void)h; SetEvent(ol->hEvent); return TRUE; }
static BOOL WINAPI mock_reset(WINUSB_INTERFACE_HANDLE h, UCHAR ep)
{ (void)h; (void)ep; return TRUE; }
static BOOL WINAPI mock_set(WINUSB_INTERFACE_HANDLE h, UCHAR ep, ULONG policy, ULONG n, PVOID value)
{
    (void)h; (void)ep; (void)n;
    if (policy == RAW_IO && !*(UCHAR *)value && fail_restore) { SetLastError(ERROR_BAD_COMMAND); return FALSE; }
    return TRUE;
}
static BOOL WINAPI mock_get(WINUSB_INTERFACE_HANDLE h, UCHAR ep, ULONG policy, PULONG n, PVOID value)
{ (void)h; (void)ep; (void)policy; *n = sizeof(maximum); *(ULONG *)value = maximum; return TRUE; }
static BOOL WINAPI mock_interface(WINUSB_INTERFACE_HANDLE h, UCHAR alt, PUSB_INTERFACE_DESCRIPTOR info)
{ (void)h; (void)alt; ZeroMemory(info, sizeof(*info)); info->bNumEndpoints = 1; return TRUE; }
static BOOL WINAPI mock_pipe(WINUSB_INTERFACE_HANDLE h, UCHAR alt, UCHAR index, PWINUSB_PIPE_INFORMATION info)
{ (void)h; (void)alt; (void)index; ZeroMemory(info, sizeof(*info)); info->PipeId = 0x84; info->PipeType = UsbdPipeTypeBulk; info->MaximumPacketSize = packet; return TRUE; }

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
    unsigned char response[8]; int len = sizeof(response);
    CHECK(itedtv_usb_ctrl_rx(&bus, response, &len) == -ETIMEDOUT);
    CHECK(outstanding == 0);
    fail_restore = true;
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == 0);
    CHECK(itedtv_usb_stop_streaming(&bus) == 0);
    CHECK(outstanding == 0 && ctx->policy_failed);
    CHECK(itedtv_usb_start_streaming(&bus, stream, ctx) == -EIO);
    CHECK(itedtv_bus_term(&bus) == 0);
    printf("winusb_lifetime_test: %s\n", checks ? "FAILED" : "PASS");
    return checks ? 1 : 0;
}
