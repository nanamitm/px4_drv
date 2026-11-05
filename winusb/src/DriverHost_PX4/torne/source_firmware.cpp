// SPDX-License-Identifier: GPL-2.0-only
#include "source_firmware.hpp"
#include "frontend_wait.hpp"
#include "../../../experimental/dt295-firmware/torne_auxiliary.h"
#include "msg.h"
#include <bcrypt.h>
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <algorithm>
#pragma comment(lib,"bcrypt.lib")
namespace px4::torne {
namespace {
using Clock=std::chrono::steady_clock;
constexpr uint32_t ram=0x17a0000,mailbox=0x17a3900,cluster=0x1700000;
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
    std::string error;
    Client(MemoryAccess &memory,WinUsbControl &control,uint32_t token,HANDLE cancel):memory_(memory),control_(control),token_(token),cancel_(cancel) {}
    bool Command(uint32_t operation,const std::vector<uint8_t> &tx={},uint32_t reads=0,uint32_t address=9,uint32_t timeout_us=500000) {
        if (!healthy) return false;
        const bool ok=Exchange(operation,tx,reads,address,timeout_us);
        // 期限切れや不明な応答を次の要求に混ぜず、再送せずにローダーの復元へ進む。
        if (!ok) healthy=false;
        return ok;
    }
private:
    bool Reject(const char *reason){error=reason;return false;}
    bool Exchange(uint32_t operation,const std::vector<uint8_t> &tx,uint32_t reads,uint32_t address,uint32_t timeout_us) {
        error.clear();
        if (tx.size()>40 || reads>40 || address>127) return Reject("argument");
        if (!timeout_us || timeout_us>5000000u) return Reject("timeout argument");
        const auto deadline=(std::min)((std::min)(Clock::now()+std::chrono::microseconds(operation==DTP_TA_CLOCK_WAIT ? timeout_us+2000000u : 2000000u),limit),control_.Deadline());
        if (Clock::now()>=deadline) return Reject("deadline before send");
        // 1要求だけ期限を狭め、終了時は親コマンドの期限とstop eventをそのまま戻す。
        struct Scope {
            WinUsbControl &control; Clock::time_point saved; HANDLE stop;
            ~Scope(){control.Begin(saved,stop);}
        } scope{control_,control_.Deadline(),control_.StopEvent()};
        control_.Begin(deadline,scope.stop);
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
            memory_.Write32(mailbox+12,sequence_)!=MemoryResult::ok) return Reject("request write");
        if (operation==DTP_TA_CLOCK_WAIT) {
            // この間はUSB要求も時計更新も発行しない。11bitの1周(2.048s)を越える。
            if (!device_clock || control_.Pause(2700)) return Reject("clock wait cancelled/deadline");
        }
        while (Clock::now()<deadline) {
            // ホストの実時間だけを供給する。時計停止時はFR側の処理上限も働く。
            if ((!device_clock && memory_.Write32(mailbox+32,Now())!=MemoryResult::ok) ||
                memory_.Transfer(mailbox,true,response.data(),64)!=MemoryResult::ok) return Reject("response read");
            if (ReadWord(response.data())!=DTP_TA_MAGIC || ReadWord(response.data()+4)!=DTP_TA_ABI) return Reject("protocol identity");
            if (ReadWord(response.data()+16)==sequence_) {
                for (unsigned i=64;i<192;i+=64)
                    if (memory_.Transfer(mailbox+i,true,response.data()+i,64)!=MemoryResult::ok) return Reject("response body");
                uint32_t held=0;
                if (memory_.Read32(mailbox+16,held)!=MemoryResult::ok || held!=sequence_) return Reject("response sequence fence");
                const auto status=ReadWord(response.data()+24),result=ReadWord(response.data()+60);
                if (status || operation!=DTP_TA_I2C)
                    std::printf("PAF2 seq=%u operation=%u status=%u result=%u features=%u tx_done=%u rx_done=%u state=%08x control=%08x\n",
                        sequence_,operation,status,result,ReadWord(response.data()+28),ReadWord(response.data()+144),
                        ReadWord(response.data()+148),ReadWord(response.data()+152),ReadWord(response.data()+156));
                // NACK/timeoutでは途中までの転送数が返る。上限を検査し、成功時だけ全数を要求する。
                const auto done_tx=ReadWord(response.data()+144),done_rx=ReadWord(response.data()+148);
                const bool counts=operation!=DTP_TA_I2C ||
                    (done_tx<=tx.size() && done_rx<=reads &&
                     ((status!=DTP_TA_OK || result!=DTP_I2C_OK) || (done_tx==tx.size() && done_rx==reads)));
                if (!counts) std::printf("PAF2 malformed completion counts seq=%u\n",sequence_);
                if(!counts)return Reject("completion counts");
                if(status!=DTP_TA_OK || result!=DTP_I2C_OK)return Reject("remote status");
                return true;
            }
            // WaitForSingleObjectの1msはWindowsのtickへ丸められるため、同じ1msを高精度timerで待つ。
            if (WaitFrontendDelay(cancel_,deadline,1000000,control_.StopEvent())) return Reject("poll cancelled/deadline");
        }
        std::printf("PAF2 request deadline expired sequence=%u operation=%u\n",sequence_,operation);
        return Reject("response deadline");
    }
public:
    bool ReadRegister(uint16_t reg,uint16_t &value) {
        if (!Command(DTP_TA_I2C,{static_cast<uint8_t>(reg>>8),static_cast<uint8_t>(reg)},2) ||
            ReadWord(response.data()+144)!=2 || ReadWord(response.data()+148)!=2) return false;
        value=static_cast<uint16_t>(uint16_t(response[104])<<8|response[105]);
        return true;
    }
};


}
struct SourceFirmware::State {
    MemoryAccess &memory;
    WinUsbControl &control;
    HANDLE file, cancel;
    WINUSB_INTERFACE_HANDLE usb;
    std::vector<uint8_t> saved=std::vector<uint8_t>(0x4000),check=std::vector<uint8_t>(0x4000);
    std::unique_ptr<Client> client;
    std::string error;
    const char *stage="idle";
    bool halt_attempted=false,backed=false,changed=false,started=false,cleanup_done=false,restored=false;
    State(MemoryAccess &m,WinUsbControl &c,HANDLE f,WINUSB_INTERFACE_HANDLE u,HANDLE stop)
        :memory(m),control(c),file(f),cancel(stop),usb(u){}
    bool Fail() {
        error=std::string(stage)+" os_error="+std::to_string(memory.LastTransportError());
        if(client)error+=" reason="+client->error+" status="+std::to_string(ReadWord(client->response.data()+24))+" result="+std::to_string(ReadWord(client->response.data()+60));
        msg_err("torne source firmware: %s\n",error.c_str());
        return false;
    }
    bool Start(const std::wstring &path,const std::wstring &expected,Clock::time_point deadline) {
        if(halt_attempted || cleanup_done){stage="already used session";return Fail();}
        stage="read image";
        std::ifstream input(std::filesystem::path(path),std::ios::binary|std::ios::ate);
        const auto length=input.tellg();
        if(!input || length<=0 || length>0x2000)return Fail();
        std::vector<uint8_t> image(static_cast<size_t>(length));
        input.seekg(0);input.read(reinterpret_cast<char*>(image.data()),length);
        if(!input)return Fail();
        stage="image SHA256";
        std::string hash;
        for(auto c:expected){if(c>=L'A'&&c<=L'F')c=static_cast<wchar_t>(c-L'A'+L'a');if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return Fail();hash+=static_cast<char>(c);}
        if(hash.size()!=64 || Sha256(image)!=hash)return Fail();
        control.Begin(deadline);
        stage="stop legacy core1";halt_attempted=true;
        if(!Halt(memory,control,0x17a3f7c))return Fail();
        stage="backup core1 RAM";
        if(!Block(memory,ram,saved,true))return Fail();
        backed=true;changed=true;
        stage="load/verify FR image";
        std::vector<uint8_t> readback(image.size()),empty(192);
        if(!Block(memory,ram,image,false)||!Block(memory,ram,readback,true)||image!=readback ||
           !Block(memory,mailbox,empty,false)||memory.Write32(cluster+0xffffc,0xa0000)!=MemoryResult::ok)return Fail();
        stage="start FR image";
        started=memory.Write32(cluster,0xfffcffff)==MemoryResult::ok;
        if(!started)return Fail();
        std::array<uint8_t,64> header{};bool ready=false;
        const auto ready_deadline=(std::min)(deadline,Clock::now()+std::chrono::seconds(2));
        while(Clock::now()<ready_deadline){
            if(memory.Transfer(mailbox,true,header.data(),header.size())!=MemoryResult::ok)return Fail();
            if(ReadWord(header.data())==DTP_TA_MAGIC && ReadWord(header.data()+4)==DTP_TA_ABI){ready=true;break;}
            if(WaitFrontendDelay(cancel,ready_deadline,1000000))return Fail();
        }
        stage="PAF2 identity";if(!ready)return Fail();
        client=std::make_unique<Client>(memory,control,GetTickCount()|1u,cancel);client->limit=deadline;
        stage="PAF2 CAPS";
        if(!client->Command(DTP_TA_PING)||!client->Command(DTP_TA_CAPS))return Fail();
        // 通常経路はホスト供給時計へフォールバックせず、機器側時計を明示的に要求する。
        client->device_clock=(ReadWord(client->response.data()+28)&DTP_TA_FEATURE_DEVICE_CLOCK)!=0;
        stage="device clock capability";
        if(!client->device_clock || ReadWord(client->response.data()+28)!=DTP_TA_FEATURE_DEVICE_CLOCK)return Fail();
        stage="device clock without host updates";
        const auto before=ReadWord(client->response.data()+164);const auto host=Clock::now();
        if(!client->Command(DTP_TA_CLOCK_WAIT,{},0,9,2500000))return Fail();
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-host).count();
        const auto delta=ReadWord(client->response.data()+164)-before;
        const auto difference=static_cast<int64_t>(delta)-elapsed;
        if(difference < -20000 || difference > 20000 || ReadWord(client->response.data()+32)!=client->published_host_us)return Fail();
        stage="prepare source I2C";
        if(!client->Command(DTP_TA_PREPARE)||ReadWord(client->response.data()+28)!=5)return Fail();
        msg_info("torne source firmware: ready bytes=%zu sha256=%s features=5 clock_host_us=%lld clock_device_us=%u\n",image.size(),hash.c_str(),static_cast<long long>(elapsed),delta);
        error.clear();return true;
    }
    bool Stop() {
        if(cleanup_done)return restored;
        cleanup_done=true;
        if(!halt_attempted){restored=true;return true;}
        // 機器handleは同じ世代に固定される。通常終了でcancelが設定済みでも別の有限期限で回収する。
        HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!event)return false;
        struct CloseEvent{HANDLE value;~CloseEvent(){CloseHandle(value);}} close{event};
        NativeControlIo io;WinUsbControl recovery(file,usb,event,io);MemoryAccess access(recovery);
        recovery.Begin(Clock::now()+std::chrono::seconds(8));
        bool ok=Halt(access,recovery,started?mailbox+8:0x17a3f7c);
        uint32_t state=0x80;
        if(ok)ok=access.Write32(0xfd8fe4,0x40)==MemoryResult::ok;
        const auto end=Clock::now()+std::chrono::milliseconds(500);
        while(ok && Clock::now()<end){ok=access.Read32(0xfd8fe0,state)==MemoryResult::ok;if(!ok || !(state&0x80))break;}
        ok=ok && !(state&0x80);
        bool exact=!changed;
        if(ok && changed && backed)exact=Block(access,ram,saved,false)&&access.Write32(cluster+0xffffc,0xa0000)==MemoryResult::ok&&Block(access,ram,check,true)&&saved==check;
        uint32_t before=0,after=0;
        const bool resumed=ok&&exact&&access.Read32(0x17a3f7c,before)==MemoryResult::ok&&access.Write32(cluster,0xfffcffff)==MemoryResult::ok&&recovery.Pause(5)==0&&access.Read32(0x17a3f7c,after)==MemoryResult::ok&&before!=after;
        restored=ok&&exact&&resumed;
        msg_info("torne source firmware: cleanup idle=%d ram_exact=%d legacy_resumed=%d restore=%d\n",ok,exact,resumed,restored);
        if(!restored)msg_err("torne source firmware: cleanup unverified; discard this session and verify USB connection before reopening.\n");
        return restored;
    }
};
SourceFirmware::SourceFirmware(MemoryAccess &m,WinUsbControl &c,HANDLE f,WINUSB_INTERFACE_HANDLE u,HANDLE cancel):state_(std::make_unique<State>(m,c,f,u,cancel)){}
SourceFirmware::~SourceFirmware(){Stop();}
bool SourceFirmware::Start(const std::wstring &p,const std::wstring &h,Clock::time_point d){try{return state_->Start(p,h,d);}catch(const std::exception &e){state_->error=e.what();return false;}}
bool SourceFirmware::Stop() noexcept {try{return state_->Stop();}catch(...){msg_err("torne source firmware: restore exception; discard this session and verify USB connection before reopening.\n");return false;}}
const std::string &SourceFirmware::Error() const noexcept{return state_->error;}
I2cResult SourceFirmware::Transfer(uint8_t address,const uint8_t *write,size_t writes,uint8_t *read,size_t reads,Clock::time_point deadline){
    auto &s=*state_;
    if(!s.client || !s.client->healthy || s.cleanup_done)return I2cResult::not_ready;
    if((address&1)||writes>32||reads>32||(!writes&&!reads)||(writes&&!write)||(reads&&!read))return I2cResult::invalid_argument;
    s.client->limit=deadline;s.stage="PAF2 transfer";
    std::vector<uint8_t> tx;if(writes)tx.assign(write,write+writes);
    if(!s.client->Command(DTP_TA_I2C,tx,static_cast<uint32_t>(reads),address>>1)){
        s.Fail();
        const auto r=ReadWord(s.client->response.data()+60);
        if(s.client->error=="remote status") {
            if(r==DTP_I2C_ADDRESS_NACK)return I2cResult::address_nack;
            if(r==DTP_I2C_DATA_NACK)return I2cResult::data_nack;
            if(r==DTP_I2C_TIMEOUT)return I2cResult::timeout;
        }
        if(s.client->error.find("deadline")!=std::string::npos)return I2cResult::timeout;
        if(s.memory.LastTransportError())return I2cResult::memory_error;
        return I2cResult::bus_error;
    }
    if(reads)std::copy_n(s.client->response.data()+104,reads,read);
    return I2cResult::ok;
}
}
