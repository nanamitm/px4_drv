// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "winusb_transport.hpp"
#include "source_firmware.hpp"
#include "msg.h"
#include "util.hpp"
#include "ts_continuity.hpp"
#include "../torne/frontend_wait.hpp"
#include <algorithm>
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
    // PIX-DT295の復調器TC90532XBGは、地デジのロック直後に訂正前のTSを数十ms出力する。
    // 同系統の復調器を持つ機種と同じく、ロックから350msまでの待機後に選局を完了し、BonDriverのPurgeで破棄させる。
    // torneは復調器が異なり実機で必要性を確認していないため、従来の動作を維持する。
    Receiver(Device &device,std::vector<uint8_t> firmware,std::unique_ptr<SourceFirmware> source):
        ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE|(device.torne_?0:RECEIVER_WAIT_AFTER_LOCK_TC_T)),
        parent_(device),firmware_(std::move(firmware)),source_(std::move(source)),transport_(device.usb_dev_.dev,device.usb_dev_.winusb,device.removed_) {}
    ~Receiver() override {Close();}
    int Open() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(open_)return -EBUSY;
        if(!parent_.available_ || !stop_.value || restore_failed_)return -ENODEV;
        UCHAR speed=0;ULONG speed_size=sizeof(speed);
        if(WinUsb_QueryDeviceInformation(parent_.usb_dev_.winusb,DEVICE_SPEED,&speed_size,&speed))
            msg_info("mAgicVISION: USB speed code=%u (1=low 2=full 3=high)\n",speed);
        auto io=transport_.Callbacks();
        int rc=source_?0:dt295_create_target(&runtime_,firmware_.data(),firmware_.size(),&io,
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
        const int rc=source_?source_->Locked(value):dt295_locked(runtime_,&value);locked=value!=0;
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
            msg_info("DT295: initializing backend=%s\n",source_?"source-fr":parent_.torne_?"torne-resident":"magicvision");
            transport_.Begin(parent_.torne_?180000:120000);const int initialized=source_?source_->Start(transport_.Callbacks()):dt295_start(runtime_);
            transport_.ReportTiming("initialize");
            if(initialized){
                msg_err("mAgicVISION: initialize failed result=%d win32=%lu\n",initialized,transport_.LastError());
                failed_=true;return initialized;
            }
            initialized_=true;
            msg_info("DT295: initialized backend=%s\n",source_?"source-fr":parent_.torne_?"torne-resident":"magicvision");
        }
        int rc=StopReader();tuned_=false;
        if(rc){failed_=true;return rc;}
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        stream_buf_->ResetInputPacketCount();worker_error_=0;
        {
            std::lock_guard<std::mutex> command(command_mutex_);
            transport_.Begin(parent_.torne_?90000:20000);
            msg_info("mAgicVISION: tune target=%s frequency_khz=%u tsid=%x\n",parent_.torne_?"torne":"DT295",params_.freq,params_.stream_id);
            const bool satellite=params_.system==px4::SystemType::ISDB_S;
            rc=source_?source_->Tune(satellite,params_.freq,params_.stream_id):
                dt295_tune(runtime_,satellite?1u:0u,params_.freq,params_.stream_id);
            if(source_ && rc && rc!=-EINVAL)failed_=true;
            transport_.ReportTiming("tune");
        }
        if(!rc){tuned_=true;if(capture_)rc=StartReader();}
        if(rc)msg_err("mAgicVISION: tune system=%u frequency_khz=%u tsid=%x result=%d\n",
            static_cast<unsigned>(params_.system),params_.freq,params_.stream_id,rc);
        return rc;
    }
private:
    void Cleanup(){
        if(source_){
            transport_.BeginRestore(120000);
            restore_failed_=source_->Close()!=0;
            transport_.ReportTiming("source-close");
            return;
        }
        if(!runtime_)return;
        const bool restore=GetEnvironmentVariableW(L"DT295_STREAM_TRACE_RESTORE",nullptr,0)>0;
        transport_.Begin(restore?180000:parent_.torne_?120000:20000);
        const int rc=restore?dt295_close_restore(runtime_):dt295_close(runtime_);
        transport_.ReportTiming("close");
        if(rc)msg_err("mAgicVISION: close failed result=%d; physical reconnection may be required\n",rc);
        dt295_destroy(runtime_);runtime_=nullptr;
    }
    int StartReader(){
        if(worker_.joinable())return worker_error_?-EIO:0;
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        worker_error_=0;deliver_=false;armed_=false;
        wchar_t trace_path[MAX_PATH]{};
        const DWORD trace_size=GetEnvironmentVariableW(L"DT295_STREAM_TRACE_DIR",trace_path,MAX_PATH);
        trace_dir_=trace_size && trace_size<MAX_PATH?trace_path:L"";
        ring_trace_=!source_ && !parent_.torne_ && GetEnvironmentVariableW(L"DT295_STREAM_TRACE_RING",nullptr,0)>0;
        service_trace_.clear();bulk_trace_.clear();trace_bytes_=0;trace_cc_=0;
        trace_origin_=Clock::now();++trace_epoch_;
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
            transport_.Begin(parent_.torne_?90000:20000);rc=source_?source_->Capture(true):dt295_capture(runtime_,1);
        }
        if(rc){StopReader();return rc;}
        // input初期化が終わる前のUSBデータは破棄し、フレーマーとの競合を避ける。
        deliver_=true;
        try {service_worker_=std::thread([this]{
            uint64_t calls=0,total_gap_us=0,max_gap_us=0,max_call_us=0;
            auto previous=Clock::now();
            while(parent_.available_ && !worker_error_){
                // SERVICEはTS送出も進める。1msをWindows tickへ丸めて約16ms遅らせない。
                // PIF1はリング/FIFOの空きを機器側で待つため、ホスト側の1ms待機を重ねない。
                const auto wait=source_?(WaitForSingleObject(stop_.value,0)==WAIT_OBJECT_0?ERROR_OPERATION_ABORTED:0):
                    px4::torne::WaitFrontendDelay(stop_.value,Clock::now()+std::chrono::seconds(2),1000000,parent_.removed_);
                if(wait==ERROR_OPERATION_ABORTED)break;
                if(wait){worker_error_=-EIO;break;}
                std::lock_guard<std::mutex> command(command_mutex_);
                const auto begin=Clock::now();
                const auto gap=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(begin-previous).count());
                total_gap_us+=gap;max_gap_us=(std::max)(max_gap_us,gap);previous=begin;++calls;
                transport_.Begin(2000);const int rc=source_?source_->Service():dt295_service(runtime_);
                const auto completed=Clock::now();
                if(!trace_dir_.empty() && service_trace_.size()<100000){
                    ServiceTrace row{};row.begin=TraceTime(begin);row.end=TraceTime(completed);
                    row.bytes=trace_bytes_.load();row.cc=trace_cc_.load();row.rc=rc;row.telemetry=transport_.Telemetry();
                    if(ring_trace_ && !rc){
                        transport_.Begin(2000);row.ring_begin=TraceTime(Clock::now());
                        row.ring_rc=dt295_diagnostic_ring(runtime_,row.ring.data());
                        row.ring_end=TraceTime(Clock::now());
                        if(row.ring_rc)worker_error_=row.ring_rc;
                    }
                    service_trace_.push_back(row);
                }
                max_call_us=(std::max)(max_call_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count()));
                if(rc)worker_error_=rc;
            }
            msg_info("mAgicVISION: service calls=%llu mean_gap_us=%llu max_gap_us=%llu max_call_us=%llu\n",
                static_cast<unsigned long long>(calls),static_cast<unsigned long long>(calls?total_gap_us/calls:0),
                static_cast<unsigned long long>(max_gap_us),static_cast<unsigned long long>(max_call_us));
        });}catch(...){StopReader();return -ENOMEM;}
        return 0;
    }
    int StopReader(){
        deliver_=false;SetEvent(stop_.value);
        if(service_worker_.joinable())service_worker_.join();
        if(worker_.joinable())worker_.join();
        SaveTrace();
        if(!source_ && (!runtime_ || !tuned_))return 0;
        std::lock_guard<std::mutex> command(command_mutex_);
        transport_.Begin(20000);const int rc=source_?source_->Capture(false):dt295_capture(runtime_,0);
        transport_.ReportTiming("capture-stop");
        if(rc)failed_=true;
        return rc;
    }
    static int Packet(void *ctx,const uint8_t *packet) noexcept {
        try {auto &out=*static_cast<std::vector<uint8_t> *>(ctx);out.insert(out.end(),packet,packet+188);return 0;}
        catch(...){return -ENOMEM;}
    }
    void ReadLoop() noexcept {
        px4::TsContinuityMonitor::Counters counts{};
        uint64_t input_bytes=0,max_read_gap_us=0;
        try {
            auto quality=std::make_unique<px4::TsContinuityMonitor>();
            std::array<std::unique_ptr<Read>,4> reads;
            for(auto &read:reads){read=std::make_unique<Read>(parent_.usb_dev_.dev,parent_.usb_dev_.winusb);if(auto error=read->Submit()){worker_error_=-EIO;break;}}
            {std::lock_guard<std::mutex> state(state_mutex_);armed_=!worker_error_;}changed_.notify_all();
            size_t head=0;auto last=Clock::now();auto warning=last;auto previous_read=last;
            std::vector<uint8_t> output;output.reserve(32768);
            while(!worker_error_){
                auto &read=*reads[head];HANDLE events[]{stop_.value,parent_.removed_,read.event.value};
                const DWORD wait=WaitForMultipleObjects(3,events,FALSE,1);
                if(wait==WAIT_OBJECT_0 || wait==WAIT_OBJECT_0+1)break;
                if(wait==WAIT_OBJECT_0+2){
                    const auto observed=Clock::now();
                    max_read_gap_us=(std::max)(max_read_gap_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(observed-previous_read).count()));
                    previous_read=observed;
                    ULONG bytes=0;
                    BOOL ok=WinUsb_GetOverlappedResult(parent_.usb_dev_.winusb,&read.operation,&bytes,TRUE);read.pending=false;
                    if(!ok || bytes>read.bytes.size()){worker_error_=-EIO;break;}
                    input_bytes+=bytes;trace_bytes_=input_bytes;
                    if(deliver_){
                        output.clear();int rc=source_?source_->Feed(read.bytes.data(),bytes,Packet,&output):
                            dt295_feed(runtime_,read.bytes.data(),bytes,Packet,&output);
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
                    if(!trace_dir_.empty() && bulk_trace_.size()<100000){
                        const auto current=quality->Counts();trace_cc_=current.cc_jump+current.repeat_changed;
                        bulk_trace_.push_back({TraceTime(observed),TraceTime(Clock::now()),bytes,trace_cc_.load()});
                    }
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
        uint64_t discarded=0,desyncs=0;
        if(source_)source_->InputStats(discarded,desyncs);
        else dt295_input_stats(runtime_,&discarded,&desyncs);
        msg_info("mAgicVISION: reader end errno=%d raw_bytes=%llu packets=%llu framing_desync=%llu discarded=%llu tei=%llu cc_jump=%llu same_cc_changed=%llu malformed=%llu max_read_gap_us=%llu\n",
            worker_error_.load(),static_cast<unsigned long long>(input_bytes),static_cast<unsigned long long>(counts.packets),
            static_cast<unsigned long long>(desyncs),static_cast<unsigned long long>(discarded),
            static_cast<unsigned long long>(counts.tei),static_cast<unsigned long long>(counts.cc_jump),
            static_cast<unsigned long long>(counts.repeat_changed),static_cast<unsigned long long>(counts.malformed),
            static_cast<unsigned long long>(max_read_gap_us));
        if(worker_error_){msg_err("mAgicVISION: stream failed errno=%d\n",worker_error_.load());stream_buf_->StopRequest();}
        changed_.notify_all();
    }
    struct ServiceTrace {int64_t begin=0,end=0,ring_begin=0,ring_end=0;uint64_t bytes=0,cc=0;int rc=0,ring_rc=0;std::array<uint32_t,4> ring{};std::array<uint32_t,10> telemetry{};};
    struct BulkTrace {int64_t completed,processed;uint64_t bytes,cc;};
    int64_t TraceTime(Clock::time_point t)const{return std::chrono::duration_cast<std::chrono::microseconds>(t-trace_origin_).count();}
    // CSV出力は受信終了後。各スレッドの専用vectorへ追記し、受信経路でファイルを待たない。
    void SaveTrace(){
        if(trace_dir_.empty() || (service_trace_.empty() && bulk_trace_.empty()))return;
        const auto stem=trace_dir_+L"\\epoch-"+std::to_wstring(trace_epoch_);
        FILE *f=nullptr;
        if(!_wfopen_s(&f,(stem+L"-service.csv").c_str(),L"wb")){
            std::fputs("begin_us,end_us,bulk_bytes,cc,rc,ring_begin_us,ring_end_us,ring_rc,base,size,producer,consumer,t_magic,t_pre_producer,t_pre_consumer,t_post_producer,t_post_consumer,t_attempts,t_success,t_fifo_busy,t_size,t_fifo_calls\n",f);
            for(const auto &r:service_trace_){std::fprintf(f,"%lld,%lld,%llu,%llu,%d,%lld,%lld,%d,%u,%u,%u,%u",(long long)r.begin,(long long)r.end,(unsigned long long)r.bytes,(unsigned long long)r.cc,r.rc,(long long)r.ring_begin,(long long)r.ring_end,r.ring_rc,r.ring[0],r.ring[1],r.ring[2],r.ring[3]);for(auto word:r.telemetry)std::fprintf(f,",%u",word);std::fputc('\n',f);}
            std::fclose(f);
        }
        if(!_wfopen_s(&f,(stem+L"-bulk.csv").c_str(),L"wb")){
            std::fputs("completed_us,processed_us,bytes,cc\n",f);
            for(const auto &r:bulk_trace_)std::fprintf(f,"%lld,%lld,%llu,%llu\n",(long long)r.completed,(long long)r.processed,(unsigned long long)r.bytes,(unsigned long long)r.cc);
            std::fclose(f);
        }
        msg_info("mAgicVISION: diagnostic trace service=%zu bulk=%zu ring=%u epoch=%u\n",service_trace_.size(),bulk_trace_.size(),ring_trace_,trace_epoch_);
        service_trace_.clear();bulk_trace_.clear();
    }
    std::wstring trace_dir_;bool ring_trace_=false;unsigned trace_epoch_=0;
    Clock::time_point trace_origin_{};
    std::vector<ServiceTrace> service_trace_;std::vector<BulkTrace> bulk_trace_;
    std::atomic<uint64_t> trace_bytes_{0},trace_cc_{0};
    Device &parent_;
    std::vector<uint8_t> firmware_;
    std::unique_ptr<SourceFirmware> source_;
    Transport transport_;
    dt295_runtime *runtime_=nullptr;
    std::mutex mutex_,command_mutex_,state_mutex_;
    std::condition_variable changed_,closed_;
    Event stop_;
    // SERVICEがUSB制御応答を待ってもBulk受信を回収し続けるため、workerを分離する。
    std::thread worker_,service_worker_;
    std::atomic_bool deliver_{false};
    std::atomic_int worker_error_{0};
    bool open_=false,capture_=false,tuned_=false,failed_=false,armed_=false,initialized_=false,restore_failed_=false;
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
    const auto backend=device_def_.configs.Get(L"FirmwareBackend",L"magicvision");
    std::vector<uint8_t> firmware;
    std::unique_ptr<SourceFirmware> source;
    if(backend==L"source-fr" && pix){
        auto path=device_def_.configs.Get(L"SourceFirmwarePath",L"");
        const auto digest=device_def_.configs.Get(L"SourceFirmwareSha256",L"");
        if(path.empty() || digest.size()!=64)return -EINVAL;
        if(!std::filesystem::path(path).is_absolute())path=px4::util::path::GetDir()+path;
        source=std::make_unique<SourceFirmware>();
        const int rc=source->Load(path,digest);
        if(rc)return rc;
    }else if(backend==L"magicvision"){
        auto path=device_def_.configs.Get(L"FirmwarePath",L"firmware\\magicvision-dt295.bin");
        if(path.empty())return -EINVAL;
        if(!std::filesystem::path(path).is_absolute())path=px4::util::path::GetDir()+path;
        const int rc=LoadFirmware(path,firmware,torne_?DT295_TARGET_TORNE:DT295_TARGET_PIX_DT295);
        if(rc){msg_err("mAgicVISION: firmware invalid/missing path=%ls result=%d\n",path.c_str(),rc);return rc;}
    }else return -EINVAL;
    receiver_=std::make_unique<Receiver>(*this,std::move(firmware),std::move(source));
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
