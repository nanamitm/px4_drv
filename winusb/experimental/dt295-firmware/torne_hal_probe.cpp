// SPDX-License-Identifier: GPL-2.0-only
// Host-executed experimental HAL; existing source-generated bridge programs only.
#include <windows.h>
#include <winusb.h>
#include <usb.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <array>
#include "../../src/DriverHost_PX4/torne/winusb_control.hpp"
#include "../../src/DriverHost_PX4/torne/dib8096_native.hpp"
#include "../../src/DriverHost_PX4/torne/frontend_wait.hpp"
#include "../../tests/torne_capture.hpp"
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

static bool Transfer(dtp_torne_hal &hal, const uint8_t *tx, uint32_t count, uint32_t reads,
    uint32_t address = 9, uint32_t timeout_us = 500000)
{
    // USBレジスターアクセスの往復を含む診断用期限。製品用の実測値ではない。
    auto r = dtp_torne_start(&hal,address,tx,count,reads,timeout_us);
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

// 診断専用。旧core1のCaptrim/TMCCジョブを使わず、通常探索の全I2CをHALへ渡す。
class HalFrontendBus final : public dibnative::Bus {
public:
    HalFrontendBus(dtp_torne_hal &hal, WinUsbControl &control, HANDLE cancel)
        : hal_(hal), control_(control), cancel_(cancel) {}
    unsigned requests = 0;
    Clock::time_point deadline{};
    void Begin(unsigned seconds) {
        deadline = Clock::now()+std::chrono::seconds(seconds);
        control_.Begin(deadline);
    }
    std::tuple<dibnative::Slice<uint8_t>,dibnative::Error> WriteRead(uint8_t address,
        dibnative::Slice<uint8_t> write, int64_t length) override {
        if (address & 1 || write.size() > 40 || length < 0 || length > 40 || (!write.size() && !length))
            return {{},dibnative::MakeError("invalid HAL frontend I2C request")};
        if (WaitForSingleObject(cancel_,0) != WAIT_TIMEOUT || Clock::now() >= deadline)
            return {{},dibnative::MakeError("HAL frontend cancelled/deadline expired")};
        uint8_t tx[40]{};
        for (size_t i=0;i<write.size();++i) tx[i]=write[static_cast<int64_t>(i)];
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline-Clock::now()).count();
        if (remaining <= 0) return {{},dibnative::MakeError("HAL frontend deadline expired")};
        const auto timeout = static_cast<uint32_t>((std::min)(remaining,int64_t(500000)));
        ++requests;
        if (!Transfer(hal_,tx,static_cast<uint32_t>(write.size()),static_cast<uint32_t>(length),address >> 1,timeout))
            return {{},dibnative::MakeError("HAL frontend I2C failed",unsigned(hal_.engine.result),"address",unsigned(address))};
        dibnative::Slice<uint8_t> response(static_cast<size_t>(length));
        for (int64_t i=0;i<length;++i) response[i]=hal_.engine.rx[i];
        return {response,nullptr};
    }
    dibnative::Error Wait(int64_t ns) override {
        const auto error=WaitFrontendDelay(cancel_,deadline,ns);
        return error ? dibnative::MakeError("HAL frontend wait failed",error) : nullptr;
    }
private:
    dtp_torne_hal &hal_;
    WinUsbControl &control_;
    HANDLE cancel_;
};

static bool Receive(dtp_torne_hal &hal, WinUsbControl &control, MemoryAccess &memory,
    Bootstrap &bootstrap, HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE cancel, const wchar_t *prefix)
{
    HalFrontendBus bus(hal,control,cancel);
    NativeFrontend frontend(bus);
    std::vector<uint8_t> response;
    auto command = [&](uint8_t kind, const std::vector<uint8_t> &body, unsigned seconds=30) {
        bus.Begin(seconds);
        const auto begin=Clock::now();
        const bool success=frontend.Command(kind,body,response);
        std::printf("FRONTEND command=%02x ok=%d elapsed_ms=%lld requests=%u error=%s\n",kind,success,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count()),
            bus.requests,frontend.Error().c_str());
        return success;
    };
    bool ok=command(0x10,{},120);
    bool strict_quality=true;
    const bool initialized=ok;
    if (ok) ok=command(0x12,{1}); // parallel gated clock
    const unsigned channels[]{13,17,13};
    const unsigned durations[]{10000,60000,10000};
    for (unsigned run=0;ok && run<3;++run) {
        const uint32_t frequency=473143000u+(channels[run]-13u)*6000000u;
        std::vector<uint8_t> tune(4);
        for (unsigned i=0;i<4;++i) tune[i]=static_cast<uint8_t>(frequency>>(i*8));
        ok=command(0x11,tune,60);
        bool locked=false;
        const auto lock_deadline=Clock::now()+std::chrono::seconds(5);
        while (ok && Clock::now()<lock_deadline) {
            ok=command(0x13,{});
            if (!ok) break;
            locked=response.size()==24 && response[0]==1;
            if (locked) break;
            ok=control.Pause(20)==0;
        }
        double cnr=0;
        if (response.size()==24) std::memcpy(&cnr,response.data()+8,8);
        std::printf("TUNE run=%u channel=%u frequency=%u locked=%d cnr_db=%.3f requests=%u\n",run,channels[run],frequency,locked,cnr,bus.requests);
        ok=ok && locked;
        if (!ok) break;
        const std::wstring path=std::wstring(prefix)+L"_gr"+std::to_wstring(channels[run])+L"_"+std::to_wstring(run)+L".ts";
        FILE *output=nullptr;
        if (_wfopen_s(&output,path.c_str(),L"wb") || !output) { ok=false; break; }
        FILE *raw_output=nullptr;
        const auto raw_path=path+L".prefix.raw";
        if (_wfopen_s(&raw_output,raw_path.c_str(),L"wb") || !raw_output) { std::fclose(output); ok=false; break; }
        size_t raw_prefix_bytes=0;
        control.Begin(Clock::now()+std::chrono::milliseconds(durations[run])+std::chrono::seconds(5));
        StreamEngine engine(memory,bootstrap);
        TorneCaptureStats stats;
        ok=CaptureFirstTs(file,usb,cancel,engine,Clock::now()+std::chrono::milliseconds(durations[run])+std::chrono::seconds(3),stats,
            durations[run],[&](const uint8_t *data,size_t bytes) { return std::fwrite(data,1,bytes,output)==bytes; },
            [&](const uint8_t *data,size_t bytes) {
                const auto saved=(std::min)(bytes,size_t(12288)-raw_prefix_bytes);
                raw_prefix_bytes+=saved;
                return std::fwrite(data,1,saved,raw_output)==saved;
            });
        ok=std::fclose(output)==0 && ok;
        ok=std::fclose(raw_output)==0 && ok;
        std::printf("CAPTURE run=%u channel=%u ok=%d packets=%llu raw_bytes=%llu tei=%llu discarded_bytes=%llu cc=%llu malformed=%llu elapsed_ms=%lld first_ts_ms=%lld max_gap_ms=%lld error=%lu prepare=%u start=%u stop=%u\n",
            run,channels[run],ok,static_cast<unsigned long long>(stats.packets),static_cast<unsigned long long>(stats.raw_bytes),
            static_cast<unsigned long long>(stats.tei),static_cast<unsigned long long>(stats.discarded_bytes),
            static_cast<unsigned long long>(stats.cc_anomalies),static_cast<unsigned long long>(stats.malformed),
            stats.elapsed_ms,stats.first_ts_ms,stats.max_read_gap_ms,stats.error,unsigned(stats.prepare),unsigned(stats.start),unsigned(stats.stop));
        // 全区間で破棄0という厳密判定を保持し、他チャンネルの診断も最後まで収集する。
        strict_quality=strict_quality && !stats.discarded_bytes;
        ok=ok && !stats.tei && !stats.cc_anomalies && !stats.malformed;
        // エラー時にも全read回収後のcore0/入力停止を、独立した期限で確認する。
        control.Begin(Clock::now()+std::chrono::seconds(3));
        const auto stopped=engine.Stop();
        std::printf("STREAM CLEANUP run=%u stop=%u\n",run,unsigned(stopped));
        ok=ok && stopped==StreamResult::ok;
    }
    // healthyなHALだけで出力をHigh-Zへ戻す。失敗したI2C操作の再送は行わない。
    if (initialized && !hal.engine.requires_reset) {
        const bool output_off=command(0x12,{0});
        const bool slept=output_off && command(0x14,{});
        ok=ok && slept;
    }
    uint32_t heartbeat=0;
    control.Begin(Clock::now()+std::chrono::seconds(3));
    const bool core_still_halted=memory.Read32(0x17a3f7c,heartbeat)==MemoryResult::ok;
    std::printf("RECEIVE END transport_ok=%d strict_zero_discard=%d requests=%u core1_heartbeat=%08x heartbeat_read=%d\n",ok,strict_quality,bus.requests,heartbeat,core_still_halted);
    return ok && strict_quality && core_still_halted;
}

static bool Run(HANDLE file, WINUSB_INTERFACE_HANDLE usb, HANDLE cancel, const wchar_t *prefix)
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
        // 識別段階は既存と同じenumeration書き込み。受信モードのRF初期化はこの後。
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
        if (ok && prefix) {
            ok=Receive(hal,control,memory,bootstrap,file,usb,cancel,prefix);
            uint32_t heartbeat=0;
            const bool unchanged=memory.Read32(0x17a3f7c,heartbeat)==MemoryResult::ok && heartbeat==after;
            std::printf("CORE1 THROUGHOUT RECEIVE unchanged=%d before=%08x after=%08x\n",unchanged,after,heartbeat);
            ok=ok && unchanged;
        }
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
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const bool receive=argc==4 && !std::wcscmp(argv[1],L"--receive");
    if (!receive && (argc != 3 || std::wcscmp(argv[1],L"--run"))) {
        std::puts("Usage: torne_hal_probe --run <exact torne interface path> OR --receive <path> <TS output prefix>; caller must release this device first.");
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
        if (ok) ok = Run(file,usb,cancel,receive?argv[3]:nullptr);
    }
    if (usb) WinUsb_Free(usb);
    if (cancel) CloseHandle(cancel);
    CloseHandle(file);
    std::puts(ok ? (receive ? "PASS: host HAL frontend and TS capture; source-built FR execution untested" :
        "PASS: host-executed source HAL physical I2C; source-built FR execution and TS untested") : "FAIL: source HAL diagnostic");
    return ok ? 0 : 1;
}
