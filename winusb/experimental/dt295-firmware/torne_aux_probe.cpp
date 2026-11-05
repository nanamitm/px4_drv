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
#include "../../src/DriverHost_PX4/torne/winusb_control.hpp"
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
public:
    std::array<uint8_t,192> response{};
    Client(MemoryAccess &memory,WinUsbControl &control,uint32_t token):memory_(memory),control_(control),token_(token) {}
    bool Command(uint32_t operation,const std::vector<uint8_t> &tx={},uint32_t reads=0,uint32_t address=9) {
        if (tx.size()>40 || reads>40 || address>127) return false;
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        control_.Begin(deadline);
        std::array<uint8_t,28> metadata{};
        WriteWord(metadata.data(),Now()); WriteWord(metadata.data()+4,token_);
        WriteWord(metadata.data()+8,token_); WriteWord(metadata.data()+12,address);
        WriteWord(metadata.data()+16,static_cast<uint32_t>(tx.size())); WriteWord(metadata.data()+20,reads);
        WriteWord(metadata.data()+24,500000);
        std::array<uint8_t,40> output{};
        std::copy(tx.begin(),tx.end(),output.begin());
        ++sequence_;
        // 不変payloadとメタデータを先に置き、sequenceを最後に公開する。
        if (memory_.Transfer(mailbox+32,false,metadata.data(),metadata.size())!=MemoryResult::ok ||
            memory_.Transfer(mailbox+64,false,output.data(),output.size())!=MemoryResult::ok ||
            memory_.Write32(mailbox+20,operation)!=MemoryResult::ok ||
            memory_.Write32(mailbox+12,sequence_)!=MemoryResult::ok) return false;
        while (Clock::now()<deadline) {
            // ホストの実時間だけを供給する。時計停止時はFR側の処理上限も働く。
            if (memory_.Write32(mailbox+32,Now())!=MemoryResult::ok ||
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
                return status==DTP_TA_OK && result==DTP_I2C_OK;
            }
            if (control_.Pause(1)) return false;
        }
        std::printf("PAF2 request deadline expired sequence=%u operation=%u\n",sequence_,operation);
        return false;
    }
    bool ReadRegister(uint16_t reg,uint16_t &value) {
        if (!Command(DTP_TA_I2C,{static_cast<uint8_t>(reg>>8),static_cast<uint8_t>(reg)},2) ||
            ReadWord(response.data()+144)!=2 || ReadWord(response.data()+148)!=2) return false;
        value=static_cast<uint16_t>(uint16_t(response[104])<<8|response[105]);
        return true;
    }
};

static bool Run(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE cancel,std::vector<uint8_t> image,const std::wstring &prefix)
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
            Client client(memory,control,GetTickCount()|1u);
            ok=client.Command(DTP_TA_PING) && client.Command(DTP_TA_CAPS);
            if (ok) ok=bootstrap.ResetDemodulator()==BootstrapResult::ok;
            if (ok) ok=client.Command(DTP_TA_PREPARE) && ReadWord(client.response.data()+28)==3;
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
            const bool quiet=ok && client.Command(DTP_TA_QUIESCE);
            ok=ok && quiet;
            std::printf("FR QUIESCE ok=%d\n",quiet);
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
    if (argc!=6 || std::wcscmp(argv[1],L"--run")) {
        std::puts("Usage: torne_aux_probe --run <exact device path> <FR binary> <expected SHA256> <backup prefix>");
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
        if (ok) ok=Run(file,usb,cancel,image,argv[5]);
    }
    if (usb) WinUsb_Free(usb);
    if (cancel) CloseHandle(cancel);
    CloseHandle(file);
    std::puts(ok ? "PASS: source-built FR PAF2 I2C, exact core1 RAM restore and legacy restart; autonomous timer and TS untested" : "FAIL: source-built FR diagnostic");
    return ok ? 0 : 1;
}
