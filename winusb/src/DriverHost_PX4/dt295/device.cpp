// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "winusb_transport.hpp"
#include "source_firmware.hpp"
#include "msg.h"
#include "util.hpp"
#include "ts_continuity.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace px4::dt295 {
namespace {
using Clock=std::chrono::steady_clock;
constexpr wchar_t DefaultDirectFirmwarePath[]=L"firmware\\dt295-direct-fr.bin";
constexpr wchar_t DefaultDirectFirmwareSha256[]=L"ba6496cf022cc3ebb1626446484b072f83fbba019d11700c1d08c1c7b75d204a";
std::int32_t CnrMilliDb(bool satellite,uint32_t raw)
{
    double cnr=0;
    if(satellite){
        if(raw<3000)return 0;
        const double p=std::sqrt(static_cast<double>(raw-3000))/64;
        cnr=(-1634.6*p*p*p*p*p)+(14341*p*p*p*p)-(50259*p*p*p)+(88977*p*p)-(89565*p)+58857;
    }else{
        if(!raw)return 0;
        const double p=10*std::log10(5505024/static_cast<double>(raw));
        cnr=(0.024*p*p*p*p)-(1.6*p*p*p)+(39.8*p*p)+(549.1*p)+3096.5;
    }
    return std::isnan(cnr)?0:static_cast<std::int32_t>(cnr);
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
    std::vector<uint8_t> bytes;
    bool pending=false;
    Read(HANDLE f,WINUSB_INTERFACE_HANDLE u,size_t size):file(f),usb(u),bytes(size){operation.hEvent=event.value;}
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
    Receiver(Device &device,std::unique_ptr<SourceFirmware> source):
        ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE|RECEIVER_WAIT_AFTER_LOCK_TC_T),
        parent_(device),source_(std::move(source)),transport_(device.usb_dev_.dev,device.usb_dev_.winusb,device.removed_) {}
    ~Receiver() override {Close();}
    int Open() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(open_)return -EBUSY;
        if(!parent_.available_ || !stop_.value || restore_failed_)return -ENODEV;
        UCHAR speed=0;ULONG speed_size=sizeof(speed);
        if(WinUsb_QueryDeviceInformation(parent_.usb_dev_.winusb,DEVICE_SPEED,&speed_size,&speed))
            msg_info("PIX-DT295: USB speed code=%u (1=low 2=full 3=high)\n",speed);
        if(!stream_buf_->Alloc(188u*65536u))return -ENOMEM;
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
        const int rc=source_->Locked(value);locked=value!=0;
        return rc;
    }
    int SetLnbVoltage(std::int32_t voltage) override {return voltage?-ENOSYS:0;}
    int ReadStat(px4::command::StatType type,std::int32_t &value) override {
        value=0;
        if(type!=px4::command::StatType::CNR)return -ENOSYS;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !parent_.available_ || failed_ || worker_error_)return -ECANCELED;
        if(!tuned_)return 0;
        // CNの読み出しはI²Cの制御転送でSERVICEを待たせる。失敗後は応答が残る可能性があるため、
        // 受信を止めずに次の選局まで読み出しを止め、TS配信を統計取得より優先する。
        if(cn_failed_)return -EIO;
        bool satellite=false;uint32_t raw=0;
        {
            ++command_waiters_;
            std::unique_lock<std::mutex> command(command_mutex_);
            --command_waiters_;
            transport_.Begin(2000);
            const int rc=source_->ReadCn(satellite,raw);
            if(rc){cn_failed_=true;return rc;}
        }
        value=CnrMilliDb(satellite,raw);
        return 0;
    }
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
            msg_info("DT295: initializing backend=%s\n",source_->Direct()?"direct-fr":"source-fr");
            transport_.Begin(120000);
            const int initialized=source_->Start(transport_.Callbacks());
            transport_.ReportTiming("initialize");
            if(initialized){
                msg_err("PIX-DT295: initialize failed result=%d win32=%lu\n",initialized,transport_.LastError());
                failed_=true;return initialized;
            }
            initialized_=true;
            msg_info("DT295: initialized backend=%s\n",source_->Direct()?"direct-fr":"source-fr");
        }
        int rc=StopReader();tuned_=false;
        if(rc){failed_=true;return rc;}
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        stream_buf_->ResetInputPacketCount();worker_error_=0;cn_failed_=false;
        uint32_t tsid=0;
        {
            std::lock_guard<std::mutex> command(command_mutex_);
            transport_.Begin(20000);
            const bool satellite=params_.system==px4::SystemType::ISDB_S;
            // 地デジの要求はSTREAM_IDを持たず、params_には直前の衛星選局のTSIDが残る。地デジでは0を使う。
            tsid=satellite?params_.stream_id:0;
            msg_info("PIX-DT295: tune frequency_khz=%u tsid=%x\n",params_.freq,tsid);
            rc=source_->Tune(satellite,params_.freq,tsid);
            // 引数不正と無信号(-ENOLINK)は機器状態を壊さないため、次の選局を受け付ける。それ以外は転送異常として扱う。
            if(rc && rc!=-EINVAL && rc!=-ENOLINK)failed_=true;
            transport_.ReportTiming("tune");
        }
        if(!rc){tuned_=true;if(capture_)rc=StartReader();}
        if(rc)msg_err("PIX-DT295: tune system=%u frequency_khz=%u tsid=%x result=%d\n",
            static_cast<unsigned>(params_.system),params_.freq,tsid,rc);
        return rc;
    }
private:
    void Cleanup(){
        transport_.BeginRestore(120000);
        restore_failed_=source_->Close()!=0;
        transport_.ReportTiming("source-close");
    }
    int StartReader(){
        if(worker_.joinable())return worker_error_?-EIO:0;
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        worker_error_=0;deliver_=false;armed_=false;
        wchar_t trace_path[MAX_PATH]{};
        const DWORD trace_size=GetEnvironmentVariableW(L"DT295_STREAM_TRACE_DIR",trace_path,MAX_PATH);
        trace_dir_=trace_size && trace_size<MAX_PATH?trace_path:L"";
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
            transport_.Begin(20000);rc=source_->Capture(true);
        }
        if(rc){StopReader();return rc;}
        // input初期化が終わる前のUSBデータは破棄し、フレーマーとの競合を避ける。
        deliver_=true;
        try {service_worker_=std::thread([this]{
            uint64_t calls=0,total_gap_us=0,max_gap_us=0,max_call_us=0;
            auto previous=Clock::now();
            while(parent_.available_ && !worker_error_){
                // PIF1は機器側でリング/FIFOを待つため、ホストでは待機を重ねない。
                if(WaitForSingleObject(stop_.value,0)==WAIT_OBJECT_0)break;
                // source-frのSERVICEは待機なしで連続するため、std::mutexの取り直しで統計要求が数秒待たされる。
                // 待機中の要求があれば1回分だけ先に通し、CNの読み出しをSERVICE1回程度の遅れに収める。
                while(command_waiters_.load() && !worker_error_ && WaitForSingleObject(stop_.value,0)!=WAIT_OBJECT_0)
                    std::this_thread::yield();
                std::lock_guard<std::mutex> command(command_mutex_);
                const auto begin=Clock::now();
                const auto gap=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(begin-previous).count());
                total_gap_us+=gap;max_gap_us=(std::max)(max_gap_us,gap);previous=begin;++calls;
                transport_.Begin(2000);const int rc=source_->Service();
                const auto completed=Clock::now();
                if(!trace_dir_.empty() && service_trace_.size()<100000){
                    ServiceTrace row{};row.begin=TraceTime(begin);row.end=TraceTime(completed);
                    row.bytes=trace_bytes_.load();row.cc=trace_cc_.load();row.rc=rc;
                    service_trace_.push_back(row);
                }
                max_call_us=(std::max)(max_call_us,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count()));
                if(rc)worker_error_=rc;
            }
            msg_info("PIX-DT295: service calls=%llu mean_gap_us=%llu max_gap_us=%llu max_call_us=%llu\n",
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
        std::lock_guard<std::mutex> command(command_mutex_);
        transport_.Begin(20000);const int rc=source_->Capture(false);
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
            const bool direct=source_->Direct();
            std::vector<std::unique_ptr<Read>> reads(direct?64:4);
            for(auto &read:reads){
                read=std::make_unique<Read>(parent_.usb_dev_.dev,parent_.usb_dev_.winusb,direct?8192:16384);
                if(auto error=read->Submit()){worker_error_=-EIO;break;}
            }
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
                        output.clear();int rc=source_->Feed(read.bytes.data(),bytes,Packet,&output);
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
                    msg_warn("PIX-DT295: no complete TS for %lld ms\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-last).count()));warning=Clock::now();
                }
            }
            counts=quality->Counts();
        }catch(...){worker_error_=-ENOMEM;}
        uint64_t discarded=0,desyncs=0;
        source_->InputStats(discarded,desyncs);
        msg_info("PIX-DT295: reader end errno=%d raw_bytes=%llu packets=%llu framing_desync=%llu discarded=%llu tei=%llu cc_jump=%llu same_cc_changed=%llu malformed=%llu max_read_gap_us=%llu\n",
            worker_error_.load(),static_cast<unsigned long long>(input_bytes),static_cast<unsigned long long>(counts.packets),
            static_cast<unsigned long long>(desyncs),static_cast<unsigned long long>(discarded),
            static_cast<unsigned long long>(counts.tei),static_cast<unsigned long long>(counts.cc_jump),
            static_cast<unsigned long long>(counts.repeat_changed),static_cast<unsigned long long>(counts.malformed),
            static_cast<unsigned long long>(max_read_gap_us));
        if(worker_error_){msg_err("PIX-DT295: stream failed errno=%d\n",worker_error_.load());stream_buf_->StopRequest();}
        changed_.notify_all();
    }
    struct ServiceTrace {int64_t begin=0,end=0;uint64_t bytes=0,cc=0;int rc=0;};
    struct BulkTrace {int64_t completed,processed;uint64_t bytes,cc;};
    int64_t TraceTime(Clock::time_point t)const{return std::chrono::duration_cast<std::chrono::microseconds>(t-trace_origin_).count();}
    // CSV出力は受信終了後。各スレッドの専用vectorへ追記し、受信経路でファイルを待たない。
    void SaveTrace(){
        if(trace_dir_.empty() || (service_trace_.empty() && bulk_trace_.empty()))return;
        const auto stem=trace_dir_+L"\\epoch-"+std::to_wstring(trace_epoch_);
        FILE *f=nullptr;
        if(!_wfopen_s(&f,(stem+L"-service.csv").c_str(),L"wb")){
            std::fputs("begin_us,end_us,bulk_bytes,cc,rc\n",f);
            for(const auto &r:service_trace_)
                std::fprintf(f,"%lld,%lld,%llu,%llu,%d\n",(long long)r.begin,(long long)r.end,
                    (unsigned long long)r.bytes,(unsigned long long)r.cc,r.rc);
            std::fclose(f);
        }
        if(!_wfopen_s(&f,(stem+L"-bulk.csv").c_str(),L"wb")){
            std::fputs("completed_us,processed_us,bytes,cc\n",f);
            for(const auto &r:bulk_trace_)std::fprintf(f,"%lld,%lld,%llu,%llu\n",(long long)r.completed,(long long)r.processed,(unsigned long long)r.bytes,(unsigned long long)r.cc);
            std::fclose(f);
        }
        msg_info("PIX-DT295: diagnostic trace service=%zu bulk=%zu epoch=%u\n",service_trace_.size(),bulk_trace_.size(),trace_epoch_);
        service_trace_.clear();bulk_trace_.clear();
    }
    std::wstring trace_dir_;unsigned trace_epoch_=0;
    Clock::time_point trace_origin_{};
    std::vector<ServiceTrace> service_trace_;std::vector<BulkTrace> bulk_trace_;
    std::atomic<uint64_t> trace_bytes_{0},trace_cc_{0};
    Device &parent_;
    std::unique_ptr<SourceFirmware> source_;
    Transport transport_;
    std::mutex mutex_,command_mutex_,state_mutex_;
    std::atomic<int> command_waiters_{0};
    std::condition_variable changed_,closed_;
    Event stop_;
    // SERVICEがUSB制御応答を待ってもBulk受信を回収し続けるため、workerを分離する。
    std::thread worker_,service_worker_;
    std::atomic_bool deliver_{false};
    std::atomic_int worker_error_{0};
    bool open_=false,capture_=false,tuned_=false,failed_=false,armed_=false,initialized_=false,restore_failed_=false,cn_failed_=false;
};

Device::Device(const std::wstring &path,const px4::DeviceDefinition &definition,std::uintptr_t index,px4::ReceiverManager &manager)
    :DeviceBase(path,definition,index,manager){
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!removed_)throw DeviceError("PIX-DT295: CreateEvent failed");
}
Device::~Device(){Term();if(removed_)CloseHandle(removed_);}
int Device::Init(){
    if(!px4::util::wtob(device_def_.configs.Get(L"Experimental",L"false")))return -EOPNOTSUPP;
    if(usb_dev_.descriptor.idVendor!=0x06b8 || usb_dev_.descriptor.idProduct!=0x103b ||
        device_def_.receivers.size()!=1)return -EINVAL;
    UCHAR alt=0xff;USB_INTERFACE_DESCRIPTOR desc{};
    if(!WinUsb_GetCurrentAlternateSetting(usb_dev_.winusb,&alt) || alt!=0 ||
        !WinUsb_QueryInterfaceSettings(usb_dev_.winusb,0,&desc) || desc.bInterfaceNumber!=0)return -EINVAL;
    bool stream=false;
    for(UCHAR i=0;i<desc.bNumEndpoints;++i){WINUSB_PIPE_INFORMATION p{};
        if(!WinUsb_QueryPipe(usb_dev_.winusb,0,i,&p))return -EIO;
        stream|=p.PipeId==0x81 && p.PipeType==UsbdPipeTypeBulk;
    }
    if(!stream)return -EINVAL;
    const auto backend=device_def_.configs.Get(L"FirmwareBackend",L"direct-fr");
    const bool direct=backend==L"direct-fr";
    if(!direct && backend!=L"source-fr"){
        msg_err("PIX-DT295: unsupported FirmwareBackend=%ls; use direct-fr or source-fr\n",backend.c_str());
        return -EINVAL;
    }
    auto path=device_def_.configs.Get(L"SourceFirmwarePath",direct?DefaultDirectFirmwarePath:L"");
    const auto digest=device_def_.configs.Get(L"SourceFirmwareSha256",direct?DefaultDirectFirmwareSha256:L"");
    if(path.empty() || digest.size()!=64)return -EINVAL;
    if(!std::filesystem::path(path).is_absolute())path=px4::util::path::GetDir()+path;
    auto source=std::make_unique<SourceFirmware>(direct);
    const int rc=source->Load(path,digest);
    if(rc)return rc;
    receiver_=std::make_unique<Receiver>(*this,std::move(source));
    px4::command::ReceiverInfo info{};const auto &r=device_def_.receivers.front();
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,r.name.c_str());info.receiver_guid=r.guid;
    info.systems=px4::SystemType::ISDB_T|px4::SystemType::ISDB_S;info.index=r.index;
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
