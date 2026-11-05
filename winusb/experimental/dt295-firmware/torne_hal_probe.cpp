// SPDX-License-Identifier: GPL-2.0-only
// Host-executed HAL diagnostic. No source-built FR firmware is loaded.
#include <windows.h>
#include <winusb.h>
#include <usb.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <array>
#include "../../src/DriverHost_PX4/torne/winusb_control.hpp"
extern "C" {
#include "torne_hal.h"
}

using namespace px4::torne;
using Clock = std::chrono::steady_clock;

struct Adapter {
    MemoryAccess &memory;
    bool exclusive = false;
    unsigned operations = 0;
    MemoryResult last = MemoryResult::ok;
    int os_error = 0;
    bool Record(MemoryResult r) {
        ++operations; last = r; os_error = memory.LastTransportError();
        return r == MemoryResult::ok;
    }
    static int Read(void *p, uint32_t a, uint32_t *v) {
        auto &s = *static_cast<Adapter *>(p); return s.Record(s.memory.Read32(a,*v));
    }
    static int Write(void *p, uint32_t a, uint32_t v) {
        auto &s = *static_cast<Adapter *>(p); return s.Record(s.memory.Write32(a,v));
    }
    static int Write16(void *p, uint32_t a, uint16_t v) {
        auto &s = *static_cast<Adapter *>(p); return s.Record(s.memory.Write16(a,v));
    }
    static uint32_t Now(void *) {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    }
    static int Owned(void *p, uint32_t generation) {
        return generation == 1 && static_cast<Adapter *>(p)->exclusive;
    }
};

static bool Transfer(dtp_torne_hal &hal, const uint8_t *tx, uint32_t count, uint32_t reads)
{
    // USBレジスターアクセスの往復を含む診断用期限。製品用の実測値ではない。
    auto r = dtp_torne_start(&hal,9,tx,count,reads,500000);
    unsigned polls = 0;
    while (r == DTP_I2C_PENDING) { r = dtp_torne_poll(&hal); ++polls; }
    if (r != DTP_I2C_OK) {
        const auto &a = *static_cast<Adapter *>(hal.bus.opaque);
        std::printf("HAL FAIL result=%u phase=%u tx_submitted=%u tx_completed=%u rx=%u state=%08x control=%08x fault_address=%08x memory=%u os_error=%d polls=%u\n",
            unsigned(r),unsigned(hal.engine.phase),hal.engine.tx_submitted,hal.engine.tx_completed,
            hal.engine.rx_completed,hal.engine.state,hal.engine.control,hal.fault_address,unsigned(a.last),a.os_error,polls);
    }
    return r == DTP_I2C_OK;
}

static bool ReadRegister(dtp_torne_hal &h, uint16_t reg, uint16_t &value)
{
    const uint8_t tx[]{static_cast<uint8_t>(reg >> 8),static_cast<uint8_t>(reg)};
    if (!Transfer(h,tx,2,2)) return false;
    value = static_cast<uint16_t>((uint16_t(h.engine.rx[0]) << 8) | h.engine.rx[1]);
    return true;
}

static bool Run(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE cancel)
{
    NativeControlIo native;
    WinUsbControl control(file,usb,cancel,native);
    control.Begin(Clock::now()+std::chrono::seconds(30));
    MemoryAccess memory(control);
    Bootstrap bootstrap(memory,control);
    // 既存の検証済みブート経路でクロックとアドレス拡張を確立する。
    const auto b = bootstrap.Initialize(GetTickCount() | 1u);
    std::printf("BOOTSTRAP result=%u memory=%u os_error=%d address=%08x\n",unsigned(b),
        unsigned(bootstrap.LastMemoryResult()),bootstrap.LastSystemError(),bootstrap.LastAddress());
    if (b != BootstrapResult::ok) {
        std::puts("Bootstrap incomplete: USB replug required before using this torne again.");
        return false;
    }

    bool ok = false, halted = false;
    uint32_t before = 0, after = 0, flags = 0;
    // 旧core1のI2Cループと新HALが同時にコントローラーを触らないよう停止する。
    const bool halt_written = memory.Write32(0x1700000,0xffffffff) == MemoryResult::ok;
    // 制御レジスターは書込み値をそのまま返さない。停止の判定は既知の停止要求と
    // 直前まで進んでいたheartbeatの停止を組み合わせ、readbackの未定義bitに依存しない。
    if (halt_written && memory.Read32(0x1700000,flags) == MemoryResult::ok &&
        memory.Read32(0x17a3f7c,before) == MemoryResult::ok &&
        control.Pause(5) == 0 && memory.Read32(0x17a3f7c,after) == MemoryResult::ok)
        halted = before == after;
    std::printf("CORE1 HALT written=%d verified=%d flags=%08x heartbeat_before=%08x after=%08x\n",
        halt_written,halted,flags,before,after);
    if (halted) {
        Adapter adapter{memory};
        adapter.exclusive = true;
        dtp_torne_bus bus{&adapter,Adapter::Read,Adapter::Write,Adapter::Write16,Adapter::Now,Adapter::Owned};
        dtp_torne_hal hal{};
        dtp_torne_init(&hal,&bus,1);
        const auto p = dtp_torne_prepare(&hal);
        std::printf("HAL PREPARE result=%u fault_address=%08x os_error=%d\n",unsigned(p),hal.fault_address,adapter.os_error);
        ok = p == DTP_I2C_OK;
        if (ok) ok = bootstrap.ResetDemodulator() == BootstrapResult::ok;
        // 既存の識別手順と同じenumeration書き込み。選局・RF初期化は対象外。
        const uint8_t enumerate[]{5,7,0,3};
        if (ok) ok = Transfer(hal,enumerate,4,0);
        uint16_t vendor = 0, device = 0;
        if (ok) ok = ReadRegister(hal,896,vendor);
        if (ok) ok = ReadRegister(hal,896,vendor) && ReadRegister(hal,897,device);
        const bool recognized = vendor == 0x01b3 &&
            (device == 0x8000 || device == 0x8001 || device == 0x8002 || device == 0x8090);
        std::printf("IDENTIFY ok=%d vendor=%04x device=%04x recognized=%d\n",ok,vendor,device,recognized);
        ok = ok && recognized;
        unsigned verified = 0;
        const auto begin = Clock::now();
        while (ok && verified < 100) {
            uint16_t v = 0, d = 0;
            ok = ReadRegister(hal,896,v) && ReadRegister(hal,897,d) && v == vendor && d == device;
            if (ok) ++verified;
        }
        std::printf("REPEAT pairs=%u target=100 ok=%d elapsed_ms=%lld adapter_operations=%u\n",
            verified,ok,static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count()),adapter.operations);
        adapter.exclusive = false;
    }
    // HALの成否と独立した後片付け期限。STOP完了を確認してから旧core1を再開する。
    control.Begin(Clock::now()+std::chrono::seconds(3));
    bool stopped = halted && memory.Write32(0xfd8fe4,0x40) == MemoryResult::ok;
    uint32_t state = 0x80;
    const auto stop_deadline = Clock::now()+std::chrono::milliseconds(500);
    while (stopped && Clock::now() < stop_deadline) {
        stopped = memory.Read32(0xfd8fe0,state) == MemoryResult::ok;
        if (!stopped || !(state & 0x80)) break;
    }
    stopped = stopped && !(state & 0x80);
    bool resumed = false;
    if (stopped && memory.Read32(0x17a3f7c,before) == MemoryResult::ok &&
        memory.Write32(0x1700000,0xfffcffff) == MemoryResult::ok && control.Pause(5) == 0 &&
        memory.Read32(0x17a3f7c,after) == MemoryResult::ok) resumed = before != after;
    std::printf("CLEANUP stop_idle=%d core1_resumed=%d state=%08x heartbeat_before=%08x after=%08x\n",stopped,resumed,state,before,after);
    if (!resumed) std::puts("USB replug required before using this torne again; recovery was not verified.");
    std::puts("Bootstrap remains in the existing native-driver-compatible state; original power-on image was not restored.");
    return ok && resumed;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3 || std::wcscmp(argv[1],L"--run")) {
        std::puts("Usage: torne_hal_probe --run <exact torne interface path>; caller must release this device first.");
        return 2;
    }
    // 共有なしのopenで、他プロセスの既存handleと後続openを両方拒否する。
    HANDLE file = CreateFileW(argv[2],GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::printf("Exclusive open failed: %lu\n",GetLastError()); return 3; }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    HANDLE cancel = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool ok = cancel && WinUsb_Initialize(file,&usb);
    if (ok) {
        USB_DEVICE_DESCRIPTOR d{}; ULONG count = 0;
        ok = WinUsb_GetDescriptor(usb,USB_DEVICE_DESCRIPTOR_TYPE,0,0,reinterpret_cast<PUCHAR>(&d),sizeof(d),&count) &&
            count == sizeof(d) && d.idVendor == 0x054c && d.idProduct == 0x04b2;
        std::printf("IDENTITY vid=%04x pid=%04x exact_match=%d exclusive_handle=1\n",d.idVendor,d.idProduct,ok);
        if (ok) ok = Run(file,usb,cancel);
    }
    if (usb) WinUsb_Free(usb);
    if (cancel) CloseHandle(cancel);
    CloseHandle(file);
    std::puts(ok ? "PASS: host-executed source HAL physical I2C; source-built FR execution and TS untested" : "FAIL: source HAL diagnostic");
    return ok ? 0 : 1;
}
