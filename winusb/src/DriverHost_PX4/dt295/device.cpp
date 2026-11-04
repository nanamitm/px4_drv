// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "winusb_transport.hpp"
#include "msg.h"
#include "util.hpp"
#include "ts_continuity.hpp"
#include <array>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace px4::dt295 {
namespace {
using Clock=std::chrono::steady_clock;
int LoadFirmware(const std::wstring &path,std::vector<uint8_t> &bytes,dt295_target target)
{
    FILE *file=nullptr;
    if(_wfopen_s(&file,path.c_str(),L"rb") || !file)return -ENOENT;
    if(fseek(file,0,SEEK_END)){fclose(file);return -EIO;}
    const long size=ftell(file);
    if(size<64 || size>1024*1024){fclose(file);return -EINVAL;}
    rewind(file);
    try {bytes.resize(static_cast<size_t>(size));}catch(...){fclose(file);return -ENOMEM;}
    const bool ok=fread(bytes.data(),1,bytes.size(),file)==bytes.size();
    fclose(file);
    return ok?dt295_validate_target(bytes.data(),bytes.size(),target):-EIO;
}
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    ~Event(){if(value)CloseHandle(value);}
    Event()=default;Event(const Event &)=delete;Event &operator=(const Event &)=delete;
};
// スレッド終了時にも未回収I/Oを残さず、DeviceBaseのUSB解放より先に回収する。
struct Read {
    HANDLE file;WINUSB_INTERFACE_HANDLE usb;
    Event event;
    OVERLAPPED operation{};
    std::array<uint8_t,16384> bytes{};
    bool pending=false;
    Read(HANDLE f,WINUSB_INTERFACE_HANDLE u):file(f),usb(u){operation.hEvent=event.value;}
    ~Read(){if(pending){CancelIoEx(file,&operation);ULONG n=0;WinUsb_GetOverlappedResult(usb,&operation,&n,TRUE);}}
    DWORD Submit(){
        if(!event.value)return ERROR_NOT_ENOUGH_MEMORY;
        ResetEvent(event.value);ULONG n=0;
        BOOL ok=WinUsb_ReadPipe(usb,0x81,bytes.data(),static_cast<ULONG>(bytes.size()),&n,&operation);
        if(!ok && GetLastError()!=ERROR_IO_PENDING)return GetLastError();
        pending=true;return 0;
    }
};
}

class Device::Receiver final : public px4::ReceiverBase {
public:
    Receiver(Device &device,std::vector<uint8_t> firmware):ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE),
        parent_(device),firmware_(std::move(firmware)),transport_(device.usb_dev_.dev,device.usb_dev_.winusb,device.removed_) {}
    ~Receiver() override {Close();}
    int Open() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(open_)return -EBUSY;
        if(!parent_.available_ || !stop_.value)return -ENODEV;
        auto io=transport_.Callbacks();
        int rc=dt295_create_target(&runtime_,firmware_.data(),firmware_.size(),&io,
            parent_.torne_?DT295_TARGET_TORNE:DT295_TARGET_PIX_DT295);
        if(!rc && !stream_buf_->Alloc(188u*65536u))rc=-ENOMEM;
        if(rc){Cleanup();msg_err("mAgicVISION: open/initialize failed result=%d win32=%lu\n",rc,transport_.LastError());return rc;}
        stream_buf_->SetThresholdSize(188u*16u);stream_buf_->Start();
        open_=true;failed_=false;worker_error_=0;capture_=tuned_=initialized_=false;return 0;
    }
    void Close() override {
        std::unique_lock<std::mutex> lock(mutex_);
        if(!open_)return;
        StopReader();Cleanup();stream_buf_->Stop();open_=false;tuned_=capture_=false;
        lock.unlock();closed_.notify_all();
    }
    void WaitClosed(){std::unique_lock<std::mutex> lock(mutex_);closed_.wait(lock,[this]{return !open_;});}
    int SetCapture(bool enable) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !parent_.available_)return -ENODEV;
        capture_=enable;
        if(!enable)return StopReader();
        if(failed_ || worker_error_)return -EIO;
        return tuned_?StartReader():0;
    }
    int CheckLock(bool &locked) override {
        std::lock_guard<std::mutex> lock(mutex_);locked=false;
        if(!open_ || !parent_.available_ || failed_ || worker_error_)return -ECANCELED;
        if(!tuned_)return 0;
        std::lock_guard<std::mutex> command(command_mutex_);
        transport_.Begin(2000);int value=0;
        const int rc=dt295_locked(runtime_,&value);locked=value!=0;
        return rc;
    }
    int SetLnbVoltage(std::int32_t voltage) override {return voltage?-ENOSYS:0;}
    int ReadStat(px4::command::StatType,std::int32_t &value) override {value=0;return -ENOSYS;}
protected:
    int SetStreamId() override {return 0;} // TSIDはSetFrequencyのTUNEへ同梱する。
    int SetFrequency() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !parent_.available_ || failed_ || worker_error_)return -ENODEV;
        if(params_.system!=px4::SystemType::ISDB_T && params_.system!=px4::SystemType::ISDB_S)return -EINVAL;
        if(params_.system==px4::SystemType::ISDB_T && params_.bandwidth &&
            params_.bandwidth!=6 && params_.bandwidth!=6000 && params_.bandwidth!=6000000)return -EINVAL;
        // ReceiverManagerはOpen中に一覧ロックを保持するため、長いUSB初期化を最初の選局まで遅延する。
        if(!initialized_){
            std::lock_guard<std::mutex> command(command_mutex_);
            // 復元前像は最初の書換え前に保存し、途中失敗やプロセス異常終了後の調査に残す。
            transport_.SetSnapshotPath(px4::util::path::GetDir()+L"mAgicVISION-preimage-"+
                std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".bin");
            msg_info("mAgicVISION: initializing resident target=%s\n",parent_.torne_?"torne":"DT295");
            transport_.Begin(parent_.torne_?180000:120000);const int initialized=dt295_start(runtime_);
            if(initialized){
                msg_err("mAgicVISION: initialize failed result=%d win32=%lu\n",initialized,transport_.LastError());
                failed_=true;return initialized;
            }
            initialized_=true;
            msg_info("mAgicVISION: resident initialized target=%s\n",parent_.torne_?"torne":"DT295");
        }
        int rc=StopReader();tuned_=false;
        if(rc){failed_=true;return rc;}
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        stream_buf_->ResetInputPacketCount();worker_error_=0;
        {
            std::lock_guard<std::mutex> command(command_mutex_);
            transport_.Begin(parent_.torne_?90000:20000);
            msg_info("mAgicVISION: tune target=%s frequency_khz=%u tsid=%x\n",parent_.torne_?"torne":"DT295",params_.freq,params_.stream_id);
            rc=dt295_tune(runtime_,params_.system==px4::SystemType::ISDB_S?1u:0u,params_.freq,params_.stream_id);
        }
        if(!rc){tuned_=true;if(capture_)rc=StartReader();}
        if(rc)msg_err("mAgicVISION: tune system=%u frequency_khz=%u tsid=%x result=%d\n",
            static_cast<unsigned>(params_.system),params_.freq,params_.stream_id,rc);
        return rc;
    }
private:
    void Cleanup(){
        if(!runtime_)return;
        transport_.Begin(parent_.torne_?120000:20000);const int rc=dt295_close(runtime_);
        if(rc)msg_err("mAgicVISION: close failed result=%d; physical reconnection may be required\n",rc);
        dt295_destroy(runtime_);runtime_=nullptr;
    }
    int StartReader(){
        if(worker_.joinable())return worker_error_?-EIO:0;
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        worker_error_=0;deliver_=false;armed_=false;
        ResetEvent(stop_.value);stream_buf_->Start();
        // STOP後のホスト側パイプ残留を捨て、新世代の読出しをTUNE前に投入する。
        if(!WinUsb_FlushPipe(parent_.usb_dev_.winusb,0x81))return -EIO;
        try{worker_=std::thread([this]{ReadLoop();});}catch(...){return -ENOMEM;}
        {
            std::unique_lock<std::mutex> state(state_mutex_);
            if(!changed_.wait_for(state,std::chrono::seconds(2),[this]{return armed_ || worker_error_.load()!=0;})){
                state.unlock();StopReader();return -ETIMEDOUT;
            }
        }
        int rc=worker_error_.load()?-EIO:0;
        if(!rc){
            std::lock_guard<std::mutex> command(command_mutex_);
            transport_.Begin(parent_.torne_?90000:20000);rc=dt295_capture(runtime_,1);
        }
        if(rc){StopReader();return rc;}
        // input初期化が終わる前のUSBデータは破棄し、フレーマーとの競合を避ける。
        deliver_=true;
        try {service_worker_=std::thread([this]{
            while(WaitForSingleObject(stop_.value,1)==WAIT_TIMEOUT && parent_.available_ && !worker_error_){
                std::lock_guard<std::mutex> command(command_mutex_);
                transport_.Begin(2000);const int rc=dt295_service(runtime_);
                if(rc)worker_error_=rc;
            }
        });}catch(...){StopReader();return -ENOMEM;}
        return 0;
    }
    int StopReader(){
        deliver_=false;SetEvent(stop_.value);
        if(service_worker_.joinable())service_worker_.join();
        if(worker_.joinable())worker_.join();
        if(!runtime_ || !tuned_)return 0;
        std::lock_guard<std::mutex> command(command_mutex_);
        transport_.Begin(20000);const int rc=dt295_capture(runtime_,0);
        if(rc)failed_=true;
        return rc;
    }
    static int Packet(void *ctx,const uint8_t *packet) noexcept {
        try {auto &out=*static_cast<std::vector<uint8_t> *>(ctx);out.insert(out.end(),packet,packet+188);return 0;}
        catch(...){return -ENOMEM;}
    }
    void ReadLoop() noexcept {
        px4::TsContinuityMonitor::Counters counts{};
        uint64_t input_bytes=0;
        try {
            auto quality=std::make_unique<px4::TsContinuityMonitor>();
            std::array<std::unique_ptr<Read>,4> reads;
            for(auto &read:reads){read=std::make_unique<Read>(parent_.usb_dev_.dev,parent_.usb_dev_.winusb);if(auto error=read->Submit()){worker_error_=-EIO;break;}}
            {std::lock_guard<std::mutex> state(state_mutex_);armed_=!worker_error_;}changed_.notify_all();
            size_t head=0;auto last=Clock::now();auto warning=last;
            std::vector<uint8_t> output;output.reserve(32768);
            while(!worker_error_){
                auto &read=*reads[head];HANDLE events[]{stop_.value,parent_.removed_,read.event.value};
                const DWORD wait=WaitForMultipleObjects(3,events,FALSE,1);
                if(wait==WAIT_OBJECT_0 || wait==WAIT_OBJECT_0+1)break;
                if(wait==WAIT_OBJECT_0+2){
                    ULONG bytes=0;
                    BOOL ok=WinUsb_GetOverlappedResult(parent_.usb_dev_.winusb,&read.operation,&bytes,TRUE);read.pending=false;
                    if(!ok || bytes>read.bytes.size()){worker_error_=-EIO;break;}
                    input_bytes+=bytes;
                    if(deliver_){
                        output.clear();int rc=dt295_feed(runtime_,read.bytes.data(),bytes,Packet,&output);
                        if(rc){worker_error_=rc;break;}
                        if(!output.empty()){
                            quality->Feed(output.data(),output.size(),[](const px4::TsContinuityMonitor::Event &){});
                            for(size_t offset=0;offset<output.size();offset+=188){
                                size_t size=188;
                                // trueの短縮は利用者のPurgeによる意図した破棄。満杯(false)と区別する。
                                if(!stream_buf_->Write(output.data()+offset,size)){worker_error_=-ENOBUFS;break;}
                            }
                            if(worker_error_)break;
                            stream_buf_->NotifyWrite();last=Clock::now();
                        }
                    }else last=Clock::now();
                    if(read.Submit()){worker_error_=-EIO;break;}head=(head+1)%reads.size();
                }else if(wait!=WAIT_TIMEOUT){worker_error_=-EIO;break;}
                if(!deliver_)last=Clock::now();
                // 実機の無入力時間は未測定のため、RF消失を任意の期限で受信停止へ変換しない。
                if(deliver_ && Clock::now()-last>std::chrono::seconds(5) && Clock::now()-warning>std::chrono::seconds(5)){
                    msg_warn("mAgicVISION: no complete TS for %lld ms\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-last).count()));warning=Clock::now();
                }
            }
            counts=quality->Counts();
        }catch(...){worker_error_=-ENOMEM;}
        uint64_t discarded=0,desyncs=0;dt295_input_stats(runtime_,&discarded,&desyncs);
        msg_info("mAgicVISION: reader end errno=%d raw_bytes=%llu packets=%llu framing_desync=%llu discarded=%llu tei=%llu cc_jump=%llu same_cc_changed=%llu malformed=%llu\n",
            worker_error_.load(),static_cast<unsigned long long>(input_bytes),static_cast<unsigned long long>(counts.packets),
            static_cast<unsigned long long>(desyncs),static_cast<unsigned long long>(discarded),
            static_cast<unsigned long long>(counts.tei),static_cast<unsigned long long>(counts.cc_jump),
            static_cast<unsigned long long>(counts.repeat_changed),static_cast<unsigned long long>(counts.malformed));
        if(worker_error_){msg_err("mAgicVISION: stream failed errno=%d\n",worker_error_.load());stream_buf_->StopRequest();}
        changed_.notify_all();
    }
    Device &parent_;
    std::vector<uint8_t> firmware_;
    Transport transport_;
    dt295_runtime *runtime_=nullptr;
    std::mutex mutex_,command_mutex_,state_mutex_;
    std::condition_variable changed_,closed_;
    Event stop_;
    // SERVICEがUSB制御応答を待ってもBulk受信を回収し続けるため、workerを分離する。
    std::thread worker_,service_worker_;
    std::atomic_bool deliver_{false};
    std::atomic_int worker_error_{0};
    bool open_=false,capture_=false,tuned_=false,failed_=false,armed_=false,initialized_=false;
};

Device::Device(const std::wstring &path,const px4::DeviceDefinition &definition,std::uintptr_t index,px4::ReceiverManager &manager)
    :DeviceBase(path,definition,index,manager){
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!removed_)throw DeviceError("mAgicVISION: CreateEvent failed");
}
Device::~Device(){Term();if(removed_)CloseHandle(removed_);}
int Device::Init(){
    if(!px4::util::wtob(device_def_.configs.Get(L"Experimental",L"false")))return -EOPNOTSUPP;
    torne_=usb_dev_.descriptor.idVendor==0x054c && usb_dev_.descriptor.idProduct==0x04b2;
    const bool pix=usb_dev_.descriptor.idVendor==0x06b8 && usb_dev_.descriptor.idProduct==0x103b;
    if((!torne_ && !pix) || device_def_.receivers.size()!=1)return -EINVAL;
    if(torne_ && !px4::util::wtob(device_def_.configs.Get(L"ResidentFirmware",L"false")))return -EINVAL;
    UCHAR alt=0xff;USB_INTERFACE_DESCRIPTOR desc{};
    if(!WinUsb_GetCurrentAlternateSetting(usb_dev_.winusb,&alt) || alt!=0 ||
        !WinUsb_QueryInterfaceSettings(usb_dev_.winusb,0,&desc) || desc.bInterfaceNumber!=0)return -EINVAL;
    bool stream=false;
    for(UCHAR i=0;i<desc.bNumEndpoints;++i){WINUSB_PIPE_INFORMATION p{};
        if(!WinUsb_QueryPipe(usb_dev_.winusb,0,i,&p))return -EIO;
        stream|=p.PipeId==0x81 && p.PipeType==UsbdPipeTypeBulk;
    }
    if(!stream)return -EINVAL;
    auto path=device_def_.configs.Get(L"FirmwarePath",L"firmware\\magicvision-dt295.bin");
    if(path.empty())return -EINVAL;
    if(!std::filesystem::path(path).is_absolute())path=px4::util::path::GetDir()+path;
    std::vector<uint8_t> firmware;const int rc=LoadFirmware(path,firmware,torne_?DT295_TARGET_TORNE:DT295_TARGET_PIX_DT295);
    if(rc){msg_err("mAgicVISION: firmware invalid/missing path=%ls result=%d\n",path.c_str(),rc);return rc;}
    receiver_=std::make_unique<Receiver>(*this,std::move(firmware));
    px4::command::ReceiverInfo info{};const auto &r=device_def_.receivers.front();
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,r.name.c_str());info.receiver_guid=r.guid;
    info.systems=torne_?px4::SystemType::ISDB_T:px4::SystemType::ISDB_T|px4::SystemType::ISDB_S;info.index=r.index;
    registered_=receiver_manager_.Register(info,receiver_.get(),GetDeviceIdentity());
    return registered_?0:-EIO;
}
void Device::SetAvailability(bool available){available_=available;if(!available && removed_)SetEvent(removed_);}
void Device::Term(){
    SetAvailability(false);
    if(registered_){receiver_manager_.Unregister(receiver_.get());registered_=false;}
    if(receiver_){receiver_->WaitClosed();receiver_.reset();}
}
px4::ReceiverBase *Device::GetReceiver(int id) const {if(id)throw std::out_of_range("PIX-DT295 receiver index");return receiver_.get();}
}
