// SPDX-License-Identifier: GPL-2.0-only
// Diagnostic loader for the source-built PAF2 core1 image; exclusive device only.
#include <windows.h>
#include <winusb.h>
#include <usb.h>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include "../../src/DriverHost_PX4/torne/winusb_control.hpp"
#include "../../src/DriverHost_PX4/torne/dib8096_native.hpp"
#include "../../src/DriverHost_PX4/torne/frontend_wait.hpp"
#include "../../tests/torne_capture.hpp"
extern "C" {
#include "torne_auxiliary.h"
}
using namespace px4::torne;
using Clock=std::chrono::steady_clock;
static constexpr uint32_t ram=0x17a0000, mailbox=0x17a3900, cluster=0x1700000;

static std::string Sha256(const std::vector<uint8_t> &bytes)
{
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD length=0,actual=0;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
        BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&actual,0)>=0) {
        std::vector<uint8_t> object(length);
        std::array<uint8_t,32> digest{};
        if (BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)>=0 &&
            BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0 &&
            BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0) {
            constexpr char digits[]="0123456789abcdef";
            for (const auto byte:digest) { result+=digits[byte>>4]; result+=digits[byte&15]; }
        }
        if (hash) BCryptDestroyHash(hash);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    return result;
}

static bool Block(MemoryAccess &memory,uint32_t address,std::vector<uint8_t> &data,bool read)
{
    for (size_t i=0;i<data.size();i+=64) {
        const auto n=(std::min)(size_t(64),data.size()-i);
        if (memory.Transfer(address+static_cast<uint32_t>(i),read,data.data()+i,n)!=MemoryResult::ok) {
            std::printf("MEMORY FAIL address=%08x read=%d os_error=%d\n",address+static_cast<uint32_t>(i),read,memory.LastTransportError());
            return false;
        }
    }
    return true;
}

static bool Save(const std::wstring &path,const std::vector<uint8_t> &data)
{
    std::ofstream file(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
    file.write(reinterpret_cast<const char *>(data.data()),static_cast<std::streamsize>(data.size()));
    file.close();
    return !file.fail();
}

static bool Halt(MemoryAccess &memory,WinUsbControl &control,uint32_t counter)
{
    uint32_t before=0,after=0;
    const bool ok=memory.Write32(cluster,0xffffffff)==MemoryResult::ok &&
        memory.Read32(counter,before)==MemoryResult::ok && control.Pause(5)==0 &&
        memory.Read32(counter,after)==MemoryResult::ok && before==after;
    std::printf("HALT ok=%d counter=%08x before=%08x after=%08x\n",ok,counter,before,after);
    return ok;
}

static uint32_t ReadWord(const uint8_t *p)
{ return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3]; }
static void WriteWord(uint8_t *p,uint32_t value)
{ for (unsigned i=0;i<4;++i) p[i]=static_cast<uint8_t>(value>>(24-8*i)); }
static uint32_t Now()
{ return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()); }

class Client {
    MemoryAccess &memory_;
    WinUsbControl &control_;
    uint32_t sequence_=0, token_;
    HANDLE cancel_;
public:
    std::array<uint8_t,192> response{};
    Clock::time_point limit=(Clock::time_point::max)();
    bool healthy=true;
    bool device_clock=false;
    uint32_t published_host_us=0;
    Client(MemoryAccess &memory,WinUsbControl &control,uint32_t token,HANDLE cancel):memory_(memory),control_(control),token_(token),cancel_(cancel) {}
    bool Command(uint32_t operation,const std::vector<uint8_t> &tx={},uint32_t reads=0,uint32_t address=9,uint32_t timeout_us=500000) {
        if (!healthy) return false;
        const bool ok=Exchange(operation,tx,reads,address,timeout_us);
        // 期限切れや不明な応答を次の要求に混ぜず、再送せずにローダーの復元へ進む。
        if (!ok) healthy=false;
        return ok;
    }
private:
    bool Exchange(uint32_t operation,const std::vector<uint8_t> &tx,uint32_t reads,uint32_t address,uint32_t timeout_us) {
        if (tx.size()>40 || reads>40 || address>127) return false;
        if (!timeout_us || timeout_us>5000000u) return false;
        const auto deadline=(std::min)(Clock::now()+std::chrono::microseconds(operation==DTP_TA_CLOCK_WAIT ? timeout_us+2000000u : 2000000u),limit);
        if (Clock::now()>=deadline) return false;
        control_.Begin(deadline);
        std::array<uint8_t,28> metadata{};
        published_host_us=Now();
        WriteWord(metadata.data(),published_host_us); WriteWord(metadata.data()+4,token_);
        WriteWord(metadata.data()+8,token_); WriteWord(metadata.data()+12,address);
        WriteWord(metadata.data()+16,static_cast<uint32_t>(tx.size())); WriteWord(metadata.data()+20,reads);
        WriteWord(metadata.data()+24,timeout_us);
        std::array<uint8_t,40> output{};
        std::copy(tx.begin(),tx.end(),output.begin());
        ++sequence_;
        // 不変payloadとメタデータを先に置き、sequenceを最後に公開する。
        if (memory_.Transfer(mailbox+32,false,metadata.data(),metadata.size())!=MemoryResult::ok ||
            memory_.Transfer(mailbox+64,false,output.data(),output.size())!=MemoryResult::ok ||
            memory_.Write32(mailbox+20,operation)!=MemoryResult::ok ||
            memory_.Write32(mailbox+12,sequence_)!=MemoryResult::ok) return false;
        if (operation==DTP_TA_CLOCK_WAIT) {
            // この間はUSB要求も時計更新も発行しない。11bitの1周(2.048s)を越える。
            if (!device_clock || control_.Pause(2700)) return false;
        }
        while (Clock::now()<deadline) {
            // ホストの実時間だけを供給する。時計停止時はFR側の処理上限も働く。
            if ((!device_clock && memory_.Write32(mailbox+32,Now())!=MemoryResult::ok) ||
                memory_.Transfer(mailbox,true,response.data(),64)!=MemoryResult::ok) return false;
            if (ReadWord(response.data())!=DTP_TA_MAGIC || ReadWord(response.data()+4)!=DTP_TA_ABI) return false;
            if (ReadWord(response.data()+16)==sequence_) {
                for (unsigned i=64;i<192;i+=64)
                    if (memory_.Transfer(mailbox+i,true,response.data()+i,64)!=MemoryResult::ok) return false;
                uint32_t held=0;
                if (memory_.Read32(mailbox+16,held)!=MemoryResult::ok || held!=sequence_) return false;
                const auto status=ReadWord(response.data()+24),result=ReadWord(response.data()+60);
                if (status || operation!=DTP_TA_I2C)
                    std::printf("PAF2 seq=%u operation=%u status=%u result=%u features=%u tx_done=%u rx_done=%u state=%08x control=%08x\n",
                        sequence_,operation,status,result,ReadWord(response.data()+28),ReadWord(response.data()+144),
                        ReadWord(response.data()+148),ReadWord(response.data()+152),ReadWord(response.data()+156));
                const auto done_tx=ReadWord(response.data()+144),done_rx=ReadWord(response.data()+148);
                const bool counts=operation!=DTP_TA_I2C ||
                    (done_tx<=tx.size() && done_rx<=reads &&
                     ((status!=DTP_TA_OK || result!=DTP_I2C_OK) || (done_tx==tx.size() && done_rx==reads)));
                if (!counts) std::printf("PAF2 malformed completion counts seq=%u\n",sequence_);
                return status==DTP_TA_OK && result==DTP_I2C_OK && counts;
            }
            // WaitForSingleObjectの1msはWindowsのtickへ丸められるため、同じ1msを高精度timerで待つ。
            if (WaitFrontendDelay(cancel_,deadline,1000000)) return false;
        }
        std::printf("PAF2 request deadline expired sequence=%u operation=%u\n",sequence_,operation);
        return false;
    }
public:
    bool ReadRegister(uint16_t reg,uint16_t &value) {
        if (!Command(DTP_TA_I2C,{static_cast<uint8_t>(reg>>8),static_cast<uint8_t>(reg)},2) ||
            ReadWord(response.data()+144)!=2 || ReadWord(response.data()+148)!=2) return false;
        value=static_cast<uint16_t>(uint16_t(response[104])<<8|response[105]);
        return true;
    }
};

// 通常探索の全転送をPAF2へ渡す。旧core1のI2C/Captrim/TMCC経路は呼ばない。
class FirmwareBus final : public dibnative::Bus {
    Client &client_;
    HANDLE cancel_;
public:
    unsigned requests=0;
    Clock::time_point deadline{};
    FirmwareBus(Client &client,HANDLE cancel):client_(client),cancel_(cancel) {}
    void Begin(unsigned seconds) { deadline=Clock::now()+std::chrono::seconds(seconds); client_.limit=deadline; }
    std::tuple<dibnative::Slice<uint8_t>,dibnative::Error> WriteRead(uint8_t address,
        dibnative::Slice<uint8_t> write,int64_t length) override {
        if ((address&1) || write.size()>40 || length<0 || length>40 || (!write.size() && !length))
            return {{},dibnative::MakeError("invalid firmware I2C request")};
        if (WaitForSingleObject(cancel_,0)!=WAIT_TIMEOUT || Clock::now()>=deadline)
            return {{},dibnative::MakeError("firmware frontend cancelled/deadline expired")};
        std::vector<uint8_t> tx(write.size());
        for (size_t i=0;i<write.size();++i) tx[i]=write[static_cast<int64_t>(i)];
        ++requests;
        if (!client_.Command(DTP_TA_I2C,tx,static_cast<uint32_t>(length),address>>1))
            return {{},dibnative::MakeError("PAF2 I2C failed","address",unsigned(address),"request",requests)};
        dibnative::Slice<uint8_t> result(static_cast<size_t>(length));
        for (int64_t i=0;i<length;++i) result[i]=client_.response[104+static_cast<size_t>(i)];
        return {result,nullptr};
    }
    dibnative::Error Wait(int64_t ns) override {
        const auto error=WaitFrontendDelay(cancel_,deadline,ns);
        return error ? dibnative::MakeError("firmware frontend wait failed",error) : nullptr;
    }
};

static bool SelfTest()
{
    struct Fake final : ControlTransport {
        std::array<uint8_t,192> bytes{};
        unsigned calls=0;
        bool bad_counts=false,bad_identity=false,io_error=false;
        int Transfer(const MemoryRequest &r,uint8_t *data,size_t size,size_t &transferred) override {
            ++calls;
            const uint32_t a=(uint32_t(r.value&0xff00)<<8)|(uint32_t(r.value&255)<<24)|r.index;
            if (io_error || a<mailbox || a-mailbox>bytes.size() || size>bytes.size()-(a-mailbox)) return ERROR_INVALID_DATA;
            const auto offset=a-mailbox;
            if (r.type==0xc0) std::copy_n(bytes.data()+offset,size,data);
            else {
                std::copy_n(data,size,bytes.data()+offset);
                if (offset==12) {
                    WriteWord(bytes.data(),bad_identity ? 0 : DTP_TA_MAGIC);
                    WriteWord(bytes.data()+4,DTP_TA_ABI);
                    WriteWord(bytes.data()+16,ReadWord(bytes.data()+12));
                    WriteWord(bytes.data()+24,0); WriteWord(bytes.data()+28,3); WriteWord(bytes.data()+60,0);
                    WriteWord(bytes.data()+144,bad_counts ? 0 : ReadWord(bytes.data()+48));
                    WriteWord(bytes.data()+148,ReadWord(bytes.data()+52));
                    bytes[104]=1; bytes[105]=0xb3;
                }
            }
            transferred=size;
            return 0;
        }
    };
    HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!cancel) return false;
    NativeControlIo native;
    WinUsbControl control(INVALID_HANDLE_VALUE,nullptr,cancel,native);
    bool ok=true;
    for (unsigned mode=0;mode<5;++mode) {
        Fake fake;
        fake.bad_counts=mode==1; fake.bad_identity=mode==2; fake.io_error=mode==3;
        MemoryAccess memory(fake);
        Client client(memory,control,17,cancel);
        if (mode==4) client.limit=Clock::now()-std::chrono::seconds(1);
        const bool success=client.Command(DTP_TA_I2C,{3,0x80},2);
        ok=ok && success==(mode==0);
        if (mode==0) ok=ok && client.response[104]==1 && client.response[105]==0xb3;
        else {
            const auto held=fake.calls;
            ok=ok && !client.healthy && !client.Command(DTP_TA_PING) && fake.calls==held;
            if (mode==4) ok=ok && held==0;
        }
    }
    {
        Fake fake; MemoryAccess memory(fake); Client client(memory,control,17,cancel);
        FirmwareBus bus(client,cancel); bus.Begin(2);
        auto result=bus.WriteRead(0x12,{3,0x80},2);
        ok=ok && !std::get<1>(result) && std::get<0>(result)[0]==1 && std::get<0>(result)[1]==0xb3 &&
            ReadWord(fake.bytes.data()+44)==9 && bus.requests==1;
        const auto held=fake.calls;
        ok=ok && bool(std::get<1>(bus.WriteRead(0x13,{3,0x80},2))) && fake.calls==held;
        ok=ok && bool(std::get<1>(bus.WriteRead(0x12,{3,0x80},41))) && fake.calls==held;
        SetEvent(cancel);
        ok=ok && bool(std::get<1>(bus.WriteRead(0x12,{3,0x80},2))) && fake.calls==held;
    }
    CloseHandle(cancel);
    std::puts(ok ? "PASS: PAF2 host counts/identity/I/O/deadline poison, address conversion, bounds and cancellation; no USB device opened" : "FAIL: PAF2 host self-test");
    return ok;
}

static bool Receive(Client &client,MemoryAccess &memory,WinUsbControl &control,Bootstrap &bootstrap,
    HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE cancel,const std::wstring &prefix)
{
    FirmwareBus bus(client,cancel);
    NativeFrontend frontend(bus);
    std::vector<uint8_t> response;
    auto command=[&](uint8_t kind,const std::vector<uint8_t> &body,unsigned seconds=30) {
        bus.Begin(seconds);
        const auto begin=Clock::now();
        const bool success=frontend.Command(kind,body,response);
        std::printf("FR FRONTEND command=%02x ok=%d elapsed_ms=%lld requests=%u error=%s\n",kind,success,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count()),
            bus.requests,frontend.Error().c_str());
        return success;
    };
    uint32_t first_heartbeat=0;
    control.Begin(Clock::now()+std::chrono::seconds(3));
    bool ok=memory.Read32(mailbox+8,first_heartbeat)==MemoryResult::ok;
    if (ok) ok=command(0x10,{},120);
    const bool initialized=ok;
    bool strict=true;
    if (ok) ok=command(0x12,{1});
    const unsigned channels[]{13,17,13},durations[]{10000,60000,10000};
    unsigned runs=0;
    for (unsigned run=0;ok && run<3;++run) {
        const uint32_t frequency=473143000u+(channels[run]-13u)*6000000u;
        std::vector<uint8_t> tune(4);
        for (unsigned i=0;i<4;++i) tune[i]=static_cast<uint8_t>(frequency>>(8*i));
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
        std::printf("FR TUNE run=%u channel=%u frequency=%u locked=%d cnr_db=%.3f requests=%u\n",run,channels[run],frequency,locked,cnr,bus.requests);
        ok=ok && locked;
        if (!ok) break;
        const auto path=prefix+L"_gr"+std::to_wstring(channels[run])+L"_"+std::to_wstring(run)+L".ts";
        FILE *output=nullptr,*raw=nullptr;
        if (_wfopen_s(&output,path.c_str(),L"wb") || !output) { ok=false; break; }
        if (_wfopen_s(&raw,(path+L".prefix.raw").c_str(),L"wb") || !raw) { std::fclose(output); ok=false; break; }
        size_t prefix_bytes=0;
        control.Begin(Clock::now()+std::chrono::milliseconds(durations[run])+std::chrono::seconds(5));
        StreamEngine engine(memory,bootstrap);
        TorneCaptureStats stats;
        ok=CaptureFirstTs(file,usb,cancel,engine,Clock::now()+std::chrono::milliseconds(durations[run])+std::chrono::seconds(3),stats,
            durations[run],[&](const uint8_t *p,size_t n) { return std::fwrite(p,1,n,output)==n; },
            [&](const uint8_t *p,size_t n) {
                const auto saved=(std::min)(n,size_t(12288)-prefix_bytes);
                prefix_bytes+=saved;
                return std::fwrite(p,1,saved,raw)==saved;
            });
        ok=std::fclose(output)==0 && ok;
        ok=std::fclose(raw)==0 && ok;
        std::printf("FR CAPTURE run=%u channel=%u ok=%d packets=%llu raw_bytes=%llu tei=%llu discarded_bytes=%llu cc=%llu malformed=%llu elapsed_ms=%lld first_ts_ms=%lld max_gap_ms=%lld error=%lu prepare=%u start=%u stop=%u\n",
            run,channels[run],ok,static_cast<unsigned long long>(stats.packets),static_cast<unsigned long long>(stats.raw_bytes),
            static_cast<unsigned long long>(stats.tei),static_cast<unsigned long long>(stats.discarded_bytes),
            static_cast<unsigned long long>(stats.cc_anomalies),static_cast<unsigned long long>(stats.malformed),
            stats.elapsed_ms,stats.first_ts_ms,stats.max_read_gap_ms,stats.error,unsigned(stats.prepare),unsigned(stats.start),unsigned(stats.stop));
        strict=strict && !stats.discarded_bytes;
        ok=ok && !stats.tei && !stats.cc_anomalies && !stats.malformed;
        control.Begin(Clock::now()+std::chrono::seconds(3));
        const auto stopped=engine.Stop();
        std::printf("FR STREAM CLEANUP run=%u stop=%u\n",run,unsigned(stopped));
        ok=ok && stopped==StreamResult::ok;
        if (ok) ++runs;
    }
    if (initialized && client.healthy) {
        const bool output_off=command(0x12,{0});
        const bool slept=output_off && command(0x14,{});
        ok=ok && slept;
    }
    client.limit=(Clock::time_point::max)();
    control.Begin(Clock::now()+std::chrono::seconds(3));
    uint32_t heartbeat=0;
    const bool alive=memory.Read32(mailbox+8,heartbeat)==MemoryResult::ok && heartbeat!=first_heartbeat;
    std::printf("FR RECEIVE END transport_ok=%d strict_zero_discard=%d runs=%u requests=%u firmware_alive=%d heartbeat=%08x->%08x\n",ok,strict,runs,bus.requests,alive,first_heartbeat,heartbeat);
    return ok && strict && runs==3 && alive;
}

static bool Run(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE cancel,std::vector<uint8_t> image,const std::wstring &prefix,bool receive,unsigned fault)
{
    NativeControlIo native;
    WinUsbControl control(file,usb,cancel,native);
    MemoryAccess memory(control);
    Bootstrap bootstrap(memory,control);
    control.Begin(Clock::now()+std::chrono::seconds(15));
    const auto boot=bootstrap.Initialize(GetTickCount()|1u);
    std::printf("BOOTSTRAP result=%u address=%08x error=%d\n",unsigned(boot),bootstrap.LastAddress(),bootstrap.LastSystemError());
    if (boot!=BootstrapResult::ok) { std::puts("USB replug required: bootstrap incomplete."); return false; }
    if (!Halt(memory,control,0x17a3f7c)) { std::puts("USB replug required: core1 stop unverified."); return false; }
    std::vector<uint8_t> saved(0x4000);
    // Bootstrap::Initializeが直前に設定した値を保存する。実機ではこのレジスターの
    // 読み返しは0になり、設定値の保存や検証には使えない。
    constexpr uint32_t vector=0xa0000;
    uint32_t vector_read=0;
    const bool backed=Block(memory,ram,saved,true) && memory.Read32(cluster+0xffffc,vector_read)==MemoryResult::ok;
    std::vector<uint8_t> vector_bytes(4);
    WriteWord(vector_bytes.data(),vector);
    const bool persisted=backed && Save(prefix+L".core1-before.bin",saved) && Save(prefix+L".vector-command.bin",vector_bytes);
    bool modified=false,started=false,ok=false;
    std::printf("BACKUP read=%d persisted=%d bytes=%zu sha256=%s vector_command=%08x vector_readback=%08x\n",backed,persisted,saved.size(),backed?Sha256(saved).c_str():"",vector,vector_read);
    if (persisted) {
        modified=true; // 途中失敗も保存前像の復元対象にする。
        std::vector<uint8_t> zero(192),readback(image.size());
        bool uploaded=Block(memory,ram,image,false) && Block(memory,ram,readback,true) && image==readback &&
            Block(memory,mailbox,zero,false) && memory.Write32(cluster+0xffffc,0xa0000)==MemoryResult::ok;
        std::printf("UPLOAD verified=%d bytes=%zu sha256=%s\n",uploaded,image.size(),Sha256(readback).c_str());
        started=uploaded && memory.Write32(cluster,0xfffcffff)==MemoryResult::ok;
        std::array<uint8_t,64> header{};
        const auto ready_deadline=Clock::now()+std::chrono::seconds(2);
        bool ready=false;
        while (started && Clock::now()<ready_deadline) {
            if (memory.Transfer(mailbox,true,header.data(),header.size())!=MemoryResult::ok) break;
            ready=ReadWord(header.data())==DTP_TA_MAGIC && ReadWord(header.data()+4)==DTP_TA_ABI;
            if (ready) break;
            if (control.Pause(1)) break;
        }
        std::printf("FR READY started=%d ready=%d magic=%08x abi=%u heartbeat=%u\n",started,ready,
            ReadWord(header.data()),ReadWord(header.data()+4),ReadWord(header.data()+8));
        if (ready) {
            Client client(memory,control,GetTickCount()|1u,cancel);
            ok=client.Command(DTP_TA_PING) && client.Command(DTP_TA_CAPS);
            client.device_clock=(ReadWord(client.response.data()+28)&DTP_TA_FEATURE_DEVICE_CLOCK)!=0;
            if (ok && client.device_clock) {
                const auto begin=Clock::now();
                const auto device_before=ReadWord(client.response.data()+164);
                ok=client.Command(DTP_TA_CLOCK_WAIT,{},0,9,2500000);
                const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count();
                const auto delta=ReadWord(client.response.data()+164)-device_before;
                const auto error=static_cast<int64_t>(delta)-elapsed;
                ok=ok && error>-20000 && error<20000 && ReadWord(client.response.data()+32)==client.published_host_us;
                std::printf("DEVICE CLOCK WAIT ok=%d host_idle_us=%lld device_delta_us=%u error_us=%lld raw_ticks=%u host_clock_word=%u\n",ok,
                    static_cast<long long>(elapsed),delta,static_cast<long long>(error),ReadWord(client.response.data()+160),ReadWord(client.response.data()+32));
            }
            if (ok) ok=bootstrap.ResetDemodulator()==BootstrapResult::ok;
            if (ok) ok=client.Command(DTP_TA_PREPARE) && ReadWord(client.response.data()+28)==(client.device_clock ? 5u : 3u);
            if (ok) ok=client.Command(DTP_TA_I2C,{5,7,0,3});
            uint16_t vendor=0,device=0;
            if (ok) ok=client.ReadRegister(896,vendor) && client.ReadRegister(896,vendor) && client.ReadRegister(897,device);
            const bool recognized=vendor==0x01b3 && (device==0x8000 || device==0x8001 || device==0x8002 || device==0x8090);
            std::printf("FR IDENTIFY ok=%d vendor=%04x device=%04x recognized=%d\n",ok,vendor,device,recognized);
            ok=ok && recognized;
            unsigned pairs=0;
            while (ok && pairs<100) {
                uint16_t v=0,d=0;
                ok=client.ReadRegister(896,v) && client.ReadRegister(897,d) && v==vendor && d==device;
                if (ok) ++pairs;
            }
            std::printf("FR REPEAT pairs=%u target=100 ok=%d\n",pairs,ok);
            if (ok && receive) ok=Receive(client,memory,control,bootstrap,file,usb,cancel,prefix);
            if (ok && fault) {
                // 固定した1アドレスだけを読む。NACKは未使用0x7f、timeoutは既知のvendorレジスター。
                const bool completed=fault==1 ? client.Command(DTP_TA_I2C,{},1,0x7f) :
                    client.Command(DTP_TA_I2C,{3,0x80},40,9,1);
                const auto expected=fault==1 ? DTP_I2C_ADDRESS_NACK : DTP_I2C_TIMEOUT;
                uint32_t sequence=0,held=0,state=0;
                const bool captured=memory.Read32(mailbox+12,sequence)==MemoryResult::ok;
                const bool rejected=!client.Command(DTP_TA_PING);
                const bool unchanged=memory.Read32(mailbox+12,held)==MemoryResult::ok && sequence==held;
                const bool idle=memory.Read32(0xfd8fe0,state)==MemoryResult::ok && !(state&0x80);
                ok=!completed && !client.healthy && ReadWord(client.response.data()+24)==DTP_TA_TRANSFER_FAILED &&
                    ReadWord(client.response.data()+60)==static_cast<uint32_t>(expected) && captured && rejected && unchanged && idle;
                std::printf("EXPECTED FAULT kind=%u status=%u result=%u poisoned=%d no_resend=%d idle=%d ok=%d\n",fault,
                    ReadWord(client.response.data()+24),ReadWord(client.response.data()+60),!client.healthy,rejected&&unchanged,idle,ok);
            }
            // 品質判定で失敗しても、通信が正常ならQUIESCEする。通信失敗時は再送せず停止へ。
            const bool quiet=fault ? ok : client.healthy && client.Command(DTP_TA_QUIESCE);
            ok=ok && quiet;
            if(fault)std::printf("FR QUIESCE skipped after expected fault; proceeding to core halt ok=%d\n",quiet);
            else std::printf("FR QUIESCE ok=%d\n",quiet);
        }
    }
    // イメージ実行に失敗しても、別の期限で停止・復元・再開を試みる。
    control.Begin(Clock::now()+std::chrono::seconds(10));
    bool stopped=Halt(memory,control,started ? mailbox+8 : 0x17a3f7c);
    uint32_t state=0x80;
    if (stopped) stopped=memory.Write32(0xfd8fe4,0x40)==MemoryResult::ok;
    const auto stop_deadline=Clock::now()+std::chrono::milliseconds(500);
    while (stopped && Clock::now()<stop_deadline) {
        stopped=memory.Read32(0xfd8fe0,state)==MemoryResult::ok;
        if (!stopped || !(state&0x80)) break;
    }
    stopped=stopped && !(state&0x80);
    bool restored=!modified;
    if (stopped && modified && backed) {
        std::vector<uint8_t> check(saved.size());
        restored=Block(memory,ram,saved,false) && memory.Write32(cluster+0xffffc,vector)==MemoryResult::ok &&
            Block(memory,ram,check,true) && saved==check;
        uint32_t read_vector=0;
        restored=restored && memory.Read32(cluster+0xffffc,read_vector)==MemoryResult::ok;
        const bool recorded=Save(prefix+L".core1-restored.bin",check);
        restored=restored && recorded;
        std::printf("RESTORE exact_ram=%d sha256=%s recorded=%d vector_command=%08x vector_readback=%08x\n",restored,Sha256(check).c_str(),recorded,vector,read_vector);
    }
    uint32_t before=0,after=0;
    const bool resumed=stopped && restored && memory.Read32(0x17a3f7c,before)==MemoryResult::ok &&
        memory.Write32(cluster,0xfffcffff)==MemoryResult::ok && control.Pause(5)==0 &&
        memory.Read32(0x17a3f7c,after)==MemoryResult::ok && before!=after;
    std::printf("CLEANUP stop_idle=%d restored=%d legacy_core1_resumed=%d heartbeat=%08x->%08x\n",stopped,restored,resumed,before,after);
    if (!resumed) std::puts("USB replug required before reusing this torne: recovery unverified.");
    std::puts("Restore baseline is the existing bootstrap-initialized core1, not the whole power-on device state.");
    return ok && resumed;
}

int wmain(int argc,wchar_t **argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc==2 && !std::wcscmp(argv[1],L"--self-test")) return SelfTest() ? 0 : 1;
    const bool receive=argc==6 && !std::wcscmp(argv[1],L"--receive");
    const unsigned fault=argc==6 && !std::wcscmp(argv[1],L"--nack") ? 1u :
        argc==6 && !std::wcscmp(argv[1],L"--timeout") ? 2u : 0u;
    if (argc!=6 || (!receive && !fault && std::wcscmp(argv[1],L"--run"))) {
        std::puts("Usage: torne_aux_probe --run|--receive|--nack|--timeout <exact device path> <FR binary> <expected SHA256> <backup/TS prefix>");
        return 2;
    }
    std::ifstream input(std::filesystem::path(argv[3]),std::ios::binary|std::ios::ate);
    const auto length=input.tellg();
    if (!input || length<=0 || length>0x2000) return 2;
    std::vector<uint8_t> image(static_cast<size_t>(length));
    input.seekg(0); input.read(reinterpret_cast<char *>(image.data()),length);
    const std::wstring expected_w(argv[4]);
    std::string expected;
    for (auto ch:expected_w) {
        if (ch>=L'A' && ch<=L'F') ch=static_cast<wchar_t>(ch-L'A'+L'a');
        if (!((ch>=L'0' && ch<=L'9') || (ch>=L'a' && ch<=L'f'))) return 2;
        expected+=static_cast<char>(ch);
    }
    const auto digest=Sha256(image);
    if (!input || digest.size()!=64 || digest!=expected) { std::puts("Image SHA256 mismatch; no device opened."); return 2; }
    std::printf("IMAGE bytes=%zu sha256=%s\n",image.size(),digest.c_str());
    HANDLE file=CreateFileW(argv[2],GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (file==INVALID_HANDLE_VALUE) { std::printf("Exclusive open failed: %lu\n",GetLastError()); return 3; }
    WINUSB_INTERFACE_HANDLE usb=nullptr;
    HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool ok=cancel && WinUsb_Initialize(file,&usb);
    if (ok) {
        USB_DEVICE_DESCRIPTOR d{}; ULONG count=0;
        ok=WinUsb_GetDescriptor(usb,USB_DEVICE_DESCRIPTOR_TYPE,0,0,reinterpret_cast<PUCHAR>(&d),sizeof(d),&count) &&
            count==sizeof(d) && d.idVendor==0x054c && d.idProduct==0x04b2;
        std::printf("IDENTITY vid=%04x pid=%04x exact_match=%d exclusive=1\n",d.idVendor,d.idProduct,ok);
        if (ok) ok=Run(file,usb,cancel,image,argv[5],receive,fault);
    }
    if (usb) WinUsb_Free(usb);
    if (cancel) CloseHandle(cancel);
    CloseHandle(file);
    std::puts(ok ? (receive ? "PASS: source-built FR PAF2 frontend/TS and core1 restore (clock mode in features)" :
        "PASS: source-built FR PAF2 I2C, exact core1 RAM restore and legacy restart (clock mode in features)") : "FAIL: source-built FR diagnostic (see transport, strict quality and restore counters)");
    return ok ? 0 : 1;
}
