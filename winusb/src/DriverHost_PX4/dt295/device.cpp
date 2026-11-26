// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "card_uart.hpp"
#include "control_channel.hpp"
#include "winusb_transport.hpp"
#include "source_firmware.hpp"
#include "stall_history.hpp"
#include "../tc90522_cnr.hpp"
#include "../stream_overflow.hpp"
#include "msg.h"
#include "util.hpp"
#include "ts_continuity.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include <vector>

namespace px4::dt295 {
namespace {
using Clock=std::chrono::steady_clock;
// C/Nの読み出しはSERVICEと同じ制御ロックを使い、1回で最大約0.2秒TSの送出を待たせる。
// アプリが信号レベルを短い間隔で求めても、読み出しはこの間隔に1回までとし、間の要求には前回の値を返す。
constexpr auto CnMinInterval=std::chrono::milliseconds(1000);
constexpr wchar_t LegacyDirectFirmwarePath[]=L"firmware\\dt295-direct-fr.bin";
constexpr wchar_t LegacyDirectFirmwareSha256[]=L"ba6496cf022cc3ebb1626446484b072f83fbba019d11700c1d08c1c7b75d204a";
constexpr wchar_t LegacySourceFirmwarePath[]=L"firmware\\dt295-source-fr.bin";
constexpr wchar_t LegacySourceFirmwareSha256[]=L"92d2373945c03bd7978ed710ba283946d355e9383f163896b2d5517c3612ccbe";
// 滞留の記録 (StallTraceDir)。カード通信のない受信試験で観測した未送信量の最大値は、地上波で約0.4 MB、
// BSで約0.3 MB、110度CSで約0.6 MBだった。1 MiBを超えた滞留だけを起点とし、通常の変動では書き出さない。
constexpr uint32_t StallHigh=1u<<20,StallLow=64u*1024u;
// 起点の後も1秒分を残し、滞留が解消していく過程のSERVICEの間隔と送出量を読めるようにする。
constexpr int64_t StallPostUs=1000000;
// 1回の受信で書き出すファイルの上限。数日の録画でも書き出し量が増え続けないようにする。
constexpr unsigned StallMaxDumps=8;
// SERVICEとBulk受信の記録は合わせて毎秒約2,000件で、起点の前の約15秒分を保持できる。
constexpr size_t StallCapacity=32768;
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    ~Event(){if(value)CloseHandle(value);}
    Event()=default;Event(const Event &)=delete;Event &operator=(const Event &)=delete;
};
struct BulkReads {
    explicit BulkReads(const std::shared_ptr<UsbHandles> &usb) : batch(std::make_unique<UsbRequests>(usb)) {}
    ~BulkReads() { DrainUsbRequests(batch); }
    std::unique_ptr<UsbRequests> batch;
};

DWORD SubmitRead(const UsbHandles &usb, UsbRequest &read)
{
    if (!usb.Ready())
        return ERROR_IO_PENDING;
    if (!read.Reset())
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!WinUsb_ReadPipe(usb.usb, 0x81, read.buffer.data(), static_cast<ULONG>(read.buffer.size()), nullptr, &read.operation)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING)
            return error;
    }
    read.pending = true;
    return ERROR_SUCCESS;
}
}

class Device::Receiver final : public px4::ReceiverBase {
public:
    // PIX-DT295の復調器TC90532XBGは、地デジのロック直後に訂正前のTSを数十ms出力する。
    // 同系統の復調器を持つ機種と同じく、ロックから350msまでの待機後に選局を完了し、BonDriverのPurgeで破棄させる。
    Receiver(Device &device,std::unique_ptr<SourceFirmware> source):
        ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE|RECEIVER_WAIT_AFTER_LOCK_TC_T),
        parent_(device),source_(std::move(source)),transport_(device.usb_,device.removed_),
        channel_(*source_,transport_,device.available_,
            [&device](unsigned ms){return WaitForSingleObject(device.removed_,ms)==WAIT_TIMEOUT?0:-ENODEV;},
            [this](ControlChannel::Hold kind,Clock::time_point begin){
                RecordHold(kind==ControlChannel::Hold::Card?StallHistory::Kind::Card:StallHistory::Kind::Cn,begin);
            }) {}
    ~Receiver() override {Close();}
    int Open() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if(open_)return -EBUSY;
        if(!parent_.available_ || !parent_.usb_->Ready() || !stop_.value || restore_failed_)return -ENODEV;
        UCHAR speed=0;ULONG speed_size=sizeof(speed);
        if(WinUsb_QueryDeviceInformation(parent_.usb_->usb,DEVICE_SPEED,&speed_size,&speed))
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
    // Init中、受信を始める前に1回だけ呼ぶ。履歴の配列はここで確保し、受信中は割り当てない。
    void EnableStallTrace(const std::wstring &directory){
        stall_dir_=directory;
        stall_history_=std::make_unique<StallHistory>(StallCapacity);
    }
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
        // Locked()はTuneで検証した状態を返すだけでUSB転送を行わない。その状態を変えるTuneとCloseは
        // mutex_の下で動くため、制御ロックを取らずに読む。制御ロックを待つと、受信中に待機なしで
        // 連続するSERVICEの後ろで数秒待たされ、選局完了の判定がTuneTimeoutを超える場合がある。
        int value=0;
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
        if(cn_cached_ && Clock::now()-cn_read_at_<CnMinInterval){value=cn_value_;return 0;}
        bool satellite=false;uint32_t raw=0;
        {
            const int rc=channel_.ReadCn(satellite,raw);
            if(rc){cn_failed_=true;return rc;}
        }
        value=px4::tc90522::CnrMilliDb(satellite,raw);
        cn_value_=value;cn_read_at_=Clock::now();cn_cached_=true;
        return 0;
    }
    // 内蔵カードUARTの操作は、受信の有無に関係なく、TSと同じ制御ロックと転送路を使う (ControlChannel::Card)。
    ControlChannel &Channel() noexcept {return channel_;}
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
            auto command=channel_.Lock();
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
        stream_buf_->ResetInputPacketCount();worker_error_=0;cn_failed_=false;cn_cached_=false;
        uint32_t tsid=0;
        {
            auto command=channel_.Lock();
            transport_.Begin(20000);
            const bool satellite=params_.system==px4::SystemType::ISDB_S;
            // 地デジの要求はSTREAM_IDを持たず、params_には直前の衛星選局のTSIDが残る。地デジでは0を使う。
            tsid=satellite?params_.stream_id:0;
            msg_info("PIX-DT295: tune frequency_khz=%u tsid=%x\n",params_.freq,tsid);
            rc=source_->Tune(satellite,params_.freq,tsid);
            // 引数不正、無信号(-ENOLINK)、存在しない相対TS番号やTSIDの不一致(-EAGAIN)は機器状態を壊さないため、
            // 次の選局を受け付ける。それ以外は転送異常として扱う。
            if(rc && rc!=-EINVAL && rc!=-ENOLINK && rc!=-EAGAIN)failed_=true;
            transport_.ReportTiming("tune");
        }
        if(!rc){tuned_=true;if(capture_)rc=StartReader();}
        if(rc)msg_err("PIX-DT295: tune system=%u frequency_khz=%u tsid=%x result=%d\n",
            static_cast<unsigned>(params_.system),params_.freq,tsid,rc);
        return rc;
    }
private:
    void Cleanup(){
        // カードの操作と転送が混ざらないよう、復元も制御ロックの下で行う。
        auto command=channel_.Lock();
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
        if(stall_history_){
            // 書き出すファイルの時刻を録画ソフトのログと照合できるよう、経過時間の起点の壁時計を残す。
            FILETIME now{};GetSystemTimePreciseAsFileTime(&now);
            stall_origin_=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
            stall_history_->Clear();stall_trigger_.Reset();
            std::lock_guard<std::mutex> lock(stall_mutex_);stall_stop_=false;stall_queue_.clear();
        }
        ResetEvent(stop_.value);stream_buf_->Start();
        // STOP後のホスト側パイプ残留を捨て、新世代の読出しをTUNE前に投入する。
        if(!parent_.usb_->Ready() || !WinUsb_FlushPipe(parent_.usb_->usb,0x81))return -EIO;
        try{worker_=std::thread([this]{ReadLoop();});}catch(...){return -ENOMEM;}
        {
            std::unique_lock<std::mutex> state(state_mutex_);
            if(!changed_.wait_for(state,std::chrono::seconds(2),[this]{return armed_ || worker_error_.load()!=0;})){
                state.unlock();StopReader();return -ETIMEDOUT;
            }
        }
        int rc=worker_error_.load()?-EIO:0;
        if(!rc){
            auto command=channel_.Lock();
            transport_.Begin(20000);rc=source_->Capture(true);
        }
        if(rc){StopReader();return rc;}
        // ファイルへの書き出しは専用スレッドで行い、SERVICEとカード通信を書き込み待ちで止めない。
        if(stall_history_){
            try{stall_writer_=std::thread([this]{StallWriter();});}catch(...){StopReader();return -ENOMEM;}
            stall_active_=true;
        }
        // input初期化が終わる前のUSBデータは破棄し、フレーマーとの競合を避ける。
        deliver_=true;
        try {service_worker_=std::thread([this]{
            ControlChannel::ServiceStats stats;
            const int rc=channel_.RunService([this]{
                return !parent_.available_ || worker_error_ || WaitForSingleObject(stop_.value,0)==WAIT_OBJECT_0;
            },[this](Clock::time_point begin,Clock::time_point completed,int rc){
                if(!trace_dir_.empty() && service_trace_.size()<100000){
                    ServiceTrace row{};row.begin=TraceTime(begin);row.end=TraceTime(completed);
                    row.bytes=trace_bytes_.load();row.cc=trace_cc_.load();row.rc=rc;
                    service_trace_.push_back(row);
                }
                if(!rc && stall_history_)RecordService(begin,completed);
            },stats);
            if(rc)worker_error_=rc;
            if(stall_active_ && stall_trigger_.Flush())QueueStallDump(true);
            msg_info("PIX-DT295: service calls=%llu mean_gap_us=%llu max_gap_us=%llu max_call_us=%llu\n",
                static_cast<unsigned long long>(stats.calls),static_cast<unsigned long long>(stats.calls?stats.total_gap_us/stats.calls:0),
                static_cast<unsigned long long>(stats.max_gap_us),static_cast<unsigned long long>(stats.max_call_us));
        });}catch(...){StopReader();return -ENOMEM;}
        return 0;
    }
    int StopReader(){
        deliver_=false;SetEvent(stop_.value);
        if(service_worker_.joinable())service_worker_.join();
        if(worker_.joinable())worker_.join();
        stall_active_=false;
        if(stall_writer_.joinable()){
            // 受信の停止までに要求された書き出しは、すべて終えてからスレッドを回収する。
            {std::lock_guard<std::mutex> lock(stall_mutex_);stall_stop_=true;}
            stall_changed_.notify_all();stall_writer_.join();
        }
        SaveTrace();
        auto command=channel_.Lock();
        transport_.Begin(20000);const int rc=source_->Capture(false);
        transport_.ReportTiming("capture-stop");
        if(rc)failed_=true;
        return rc;
    }
    void ReadLoop() noexcept {
        px4::TsContinuityMonitor::Counters counts{};
        uint64_t input_bytes=0,max_read_gap_us=0;
        try {
            TsDelivery delivery;
            const bool direct=source_->Direct();
            BulkReads pending(parent_.usb_);
            auto &reads = pending.batch->requests;
            const std::size_t count = direct ? 64 : 4;
            reads.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                reads.push_back(std::make_unique<UsbRequest>(direct ? 8192 : 16384));
                if (SubmitRead(*parent_.usb_, *reads.back())) {
                    worker_error_ = -EIO;
                    break;
                }
            }
            {std::lock_guard<std::mutex> state(state_mutex_);armed_=!worker_error_;}changed_.notify_all();
            size_t head=0;auto last=Clock::now();auto warning=last;auto previous_read=last;
            px4::StreamOverflowLog overflow("PIX-DT295");
            const auto deliver=[this,&overflow,&last](const uint8_t *packets,size_t size){
                for(size_t offset=0;offset<size;offset+=188){
                    size_t written_size=188;
                    const bool written=stream_buf_->Write(packets+offset,written_size);
                    overflow.Record(written,188,written_size);
                }
                stream_buf_->NotifyWrite();last=Clock::now();
            };
            while(!worker_error_){
                auto &read=*reads[head];HANDLE events[]{stop_.value,parent_.removed_,read.operation.hEvent};
                const DWORD wait=WaitForMultipleObjects(3,events,FALSE,1);
                if(wait==WAIT_OBJECT_0 || wait==WAIT_OBJECT_0+1)break;
                if(wait==WAIT_OBJECT_0+2){
                    const auto observed=Clock::now();
                    const auto read_gap_us=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(observed-previous_read).count());
                    max_read_gap_us=(std::max)(max_read_gap_us,read_gap_us);
                    previous_read=observed;
                    ULONG bytes=0;
                    const BOOL ok = WinUsb_GetOverlappedResult(parent_.usb_->usb, &read.operation, &bytes, FALSE);
                    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
                    read.pending = error == ERROR_IO_INCOMPLETE || error == ERROR_IO_PENDING;
                    if(!ok || bytes>read.buffer.size()){worker_error_=-EIO;break;}
                    if(stall_active_)
                        stall_history_->Record({TraceTime(observed),static_cast<uint32_t>((std::min<uint64_t>)(read_gap_us,UINT32_MAX)),
                            static_cast<uint32_t>(bytes),0,StallHistory::Kind::Bulk});
                    input_bytes+=bytes;trace_bytes_=input_bytes;
                    if(deliver_){
                        const int rc=delivery.Deliver(*source_,read.buffer.data(),bytes,deliver);
                        if(rc){worker_error_=rc;break;}
                    }else last=Clock::now();
                    if(!trace_dir_.empty() && bulk_trace_.size()<100000){
                        const auto current=delivery.Counts();trace_cc_=current.cc_jump+current.repeat_changed;
                        bulk_trace_.push_back({TraceTime(observed),TraceTime(Clock::now()),bytes,trace_cc_.load()});
                    }
                    if(SubmitRead(*parent_.usb_,read)){worker_error_=-EIO;break;}head=(head+1)%reads.size();
                }else if(wait!=WAIT_TIMEOUT){worker_error_=-EIO;break;}
                if(!deliver_)last=Clock::now();
                // 実機の無入力時間は未測定のため、RF消失を任意の期限で受信停止へ変換しない。
                if(deliver_ && Clock::now()-last>std::chrono::seconds(5) && Clock::now()-warning>std::chrono::seconds(5)){
                    msg_warn("PIX-DT295: no complete TS for %lld ms\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-last).count()));warning=Clock::now();
                }
            }
            counts=delivery.Counts();
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
    // 滞留の記録 (StallTraceDir)。受信中だけ記録し、SERVICEの後で起点と書き出しを判定する。
    struct StallDump {std::vector<StallHistory::Event> events;int64_t trigger_us=0;uint32_t peak=0;unsigned epoch=0,index=0;bool at_stop=false;};
    static uint32_t Micros(Clock::duration d){
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(d).count();
        return us<=0?0u:static_cast<uint32_t>((std::min<long long>)(us,UINT32_MAX));
    }
    void RecordHold(StallHistory::Kind kind,Clock::time_point begin){
        if(!stall_active_)return;
        stall_history_->Record({TraceTime(begin),Micros(Clock::now()-begin),0,0,kind});
    }
    // 制御ロックを保持したSERVICEのスレッドから呼ぶ。起点の判定はこのスレッドだけが行う。
    void RecordService(Clock::time_point begin,Clock::time_point completed) noexcept {
        if(!stall_active_)return;
        const auto pending=source_->LastPending();
        stall_history_->Record({TraceTime(begin),Micros(completed-begin),source_->LastSentBlocks(),pending,StallHistory::Kind::Service});
        if(stall_trigger_.OnService(TraceTime(completed),pending))QueueStallDump(false);
    }
    // at_stop: 起点から1秒経つ前に受信が止まった。Bulk受信の停止後に終わったSERVICEを含む場合があるため、
    // 受信中の滞留と区別できるよう書き出す。
    void QueueStallDump(bool at_stop) noexcept {
        try {
            StallDump dump{stall_history_->Snapshot(),stall_trigger_.TriggerTime(),stall_trigger_.Peak(),trace_epoch_,stall_trigger_.Dumps(),at_stop};
            std::lock_guard<std::mutex> lock(stall_mutex_);
            // 書き出しが追いつかない場合も、受信経路では待たずに後続の要求を捨てる。
            if(stall_queue_.size()<2)stall_queue_.push_back(std::move(dump));
        }catch(...){return;}
        stall_changed_.notify_one();
    }
    void StallWriter(){
        std::unique_lock<std::mutex> lock(stall_mutex_);
        for(;;){
            stall_changed_.wait(lock,[this]{return stall_stop_ || !stall_queue_.empty();});
            if(stall_queue_.empty())return;
            StallDump dump=std::move(stall_queue_.front());stall_queue_.erase(stall_queue_.begin());
            lock.unlock();WriteStallDump(dump);lock.lock();
        }
    }
    // 受信開始からの経過時間をローカル時刻へ変換する。
    SYSTEMTIME StallClock(int64_t elapsed_us)const{
        const uint64_t value=stall_origin_+static_cast<uint64_t>((std::max<int64_t>)(elapsed_us,0))*10u;
        FILETIME utc{static_cast<DWORD>(value),static_cast<DWORD>(value>>32)},local{};SYSTEMTIME st{};
        FileTimeToLocalFileTime(&utc,&local);FileTimeToSystemTime(&local,&st);
        return st;
    }
    void WriteStallDump(const StallDump &dump) noexcept {
        try {
            std::wstring dir=stall_dir_;
            // 相対パスは、DebugLogPathと同じくDriverHost_PX4.exeのフォルダーを基準にする。
            const bool absolute=dir.size()>=2 && (dir[1]==L':' || (dir[0]==L'\\' && dir[1]==L'\\'));
            if(!absolute)dir=px4::util::path::GetDir()+dir;
            CreateDirectoryW(dir.c_str(),nullptr);
            const auto at=StallClock(dump.trigger_us);
            wchar_t name[80]{};
            swprintf_s(name,L"\\dt295-stall-%04u%02u%02u-%02u%02u%02u-%03u.csv",at.wYear,at.wMonth,at.wDay,
                at.wHour,at.wMinute,at.wSecond,at.wMilliseconds);
            const auto path=dir+name;
            FILE *f=nullptr;
            if(_wfopen_s(&f,path.c_str(),L"wb") || !f){msg_err("PIX-DT295: stall trace write failed path=%ls\n",path.c_str());return;}
            const auto origin=StallClock(0);
            std::fprintf(f,"# origin_local=%04u-%02u-%02u %02u:%02u:%02u.%03u trigger_us=%lld peak_pending=%u epoch=%u index=%u reason=%s\n",
                origin.wYear,origin.wMonth,origin.wDay,origin.wHour,origin.wMinute,origin.wSecond,origin.wMilliseconds,
                static_cast<long long>(dump.trigger_us),dump.peak,dump.epoch,dump.index,dump.at_stop?"stop":"threshold");
            std::fputs("# S: duration=SERVICE call, a=sent blocks (512 bytes), b=pending bytes after the SERVICE\n"
                "# C/N: duration=control lock held by a card operation / CN read\n"
                "# B: duration=gap since the previous bulk completion, a=bytes\n"
                "time_us,kind,duration_us,a,b\n",f);
            for(const auto &e:dump.events)
                std::fprintf(f,"%lld,%c,%u,%u,%u\n",static_cast<long long>(e.time_us),static_cast<char>(e.kind),e.duration_us,e.a,e.b);
            std::fclose(f);
            msg_warn("PIX-DT295: TS ring stall trace reason=%s peak_pending=%u at=%02u:%02u:%02u.%03u events=%zu file=%ls\n",
                dump.at_stop?"stop":"threshold",dump.peak,at.wHour,at.wMinute,at.wSecond,at.wMilliseconds,dump.events.size(),path.c_str());
        }catch(...){}
    }
    std::unique_ptr<StallHistory> stall_history_;std::wstring stall_dir_;
    StallTrigger stall_trigger_{StallHigh,StallLow,StallPostUs,StallMaxDumps};
    uint64_t stall_origin_=0;
    std::atomic_bool stall_active_{false};
    std::thread stall_writer_;
    std::mutex stall_mutex_;std::condition_variable stall_changed_;
    std::vector<StallDump> stall_queue_;bool stall_stop_=false;
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
    // 制御ロック、SERVICEのループ、カード操作。Linux版と共有する。
    ControlChannel channel_;
    std::mutex mutex_,state_mutex_;
    std::condition_variable changed_,closed_;
    Event stop_;
    // SERVICEがUSB制御応答を待ってもBulk受信を回収し続けるため、workerを分離する。
    std::thread worker_,service_worker_;
    std::atomic_bool deliver_{false};
    std::atomic_int worker_error_{0};
    bool open_=false,capture_=false,tuned_=false,failed_=false,armed_=false,initialized_=false,restore_failed_=false,cn_failed_=false,cn_cached_=false;
    // 選局ごとに破棄する直近のC/Nと読み出し時刻 (mutex_で保護)
    std::int32_t cn_value_=0;Clock::time_point cn_read_at_{};
};

Device::Device(const std::wstring &path,const px4::DeviceDefinition &definition,std::uintptr_t index,px4::ReceiverManager &manager)
    :DeviceBase(path,definition,index,manager){
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!removed_)throw DeviceError("PIX-DT295: CreateEvent failed");
}
Device::~Device(){Term();if(removed_)CloseHandle(removed_);}
int Device::Init(){
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
    const auto path=device_def_.configs.Get(L"SourceFirmwarePath",L"");
    const auto digest=device_def_.configs.Get(L"SourceFirmwareSha256",L"");
    // 旧配布INIの既定の組も、外部ファイルを必要とせずに移行できる。
    const auto legacy_path=direct ? LegacyDirectFirmwarePath : LegacySourceFirmwarePath;
    const auto legacy_digest=direct ? LegacyDirectFirmwareSha256 : LegacySourceFirmwareSha256;
    if ((!path.empty() || !digest.empty()) && (path != legacy_path || digest != legacy_digest)) {
        msg_err("PIX-DT295: external firmware support has been removed. Remove SourceFirmwarePath and SourceFirmwareSha256 to use embedded %ls firmware.\n", backend.c_str());
        return -EINVAL;
    }
    auto source=std::make_unique<SourceFirmware>(direct);
    const auto rc=source->LoadEmbedded();
    if(rc)return rc;
    usb_ = std::make_shared<UsbHandles>(usb_dev_.dev, usb_dev_.winusb);
    usb_dev_.dev = INVALID_HANDLE_VALUE;
    usb_dev_.winusb = nullptr;
    receiver_=std::make_unique<Receiver>(*this,std::move(source));
    // 空なら記録しない。TSリングの滞留の前後を、受信と別のスレッドでこのフォルダーへ書き出す。
    const auto stall_dir=device_def_.configs.Get(L"StallTraceDir",L"");
    if(!stall_dir.empty()){
        receiver_->EnableStallTrace(stall_dir);
        msg_info("PIX-DT295: stall trace enabled dir=%ls high=%u\n",stall_dir.c_str(),StallHigh);
    }
    px4::command::ReceiverInfo info{};const auto &r=device_def_.receivers.front();
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,r.name.c_str());info.receiver_guid=r.guid;
    info.systems=px4::SystemType::ISDB_T|px4::SystemType::ISDB_S;info.index=r.index;
    registered_=receiver_manager_.Register(info,receiver_.get(),GetDeviceIdentity());
    card_reader_=registered_;
    return registered_?0:-EIO;
}
void Device::SetAvailability(bool available){available_=available;if(!available && removed_)SetEvent(removed_);}
void Device::Term(){
    SetAvailability(false);
    if(registered_){receiver_manager_.Unregister(receiver_.get());registered_=false;}
    // 実行中のカード操作がreceiver_を使い終わるまで待ってから破棄する。
    std::lock_guard<std::mutex> card(card_mutex_);
    card_open_=false;card_.Discard();
    if(receiver_){receiver_->WaitClosed();receiver_.reset();}
}

// カードの応答の状態とUARTの手順はLinux版と共有する (CardSession)。ここではDriverHostのカードAPIの
// 開閉と利用可否の判定だけを行う。
int Device::OpenCard(){
    std::lock_guard<std::mutex> lock(card_mutex_);
    if(!receiver_ || !available_)return -ENODEV;
    if(card_open_)return -EBUSY;
    card_open_=true;card_.Discard();
    return 0;
}

void Device::CloseCard(){
    std::lock_guard<std::mutex> lock(card_mutex_);
    if(!card_open_)return;
    card_open_=false;card_.Discard();
    if(available_ && receiver_ && card_.PowerDown(receiver_->Channel()))
        msg_warn("PIX-DT295: card power down failed\n");
}

int Device::DetectCard(bool &detected){
    std::lock_guard<std::mutex> lock(card_mutex_);
    detected=false;
    if(!card_open_ || !available_ || !receiver_)return -ENODEV;
    return card_.Detect(receiver_->Channel(),detected);
}

int Device::ResetCard(){
    std::lock_guard<std::mutex> lock(card_mutex_);
    if(!card_open_ || !available_ || !receiver_)return -ENODEV;
    return card_.Reset(receiver_->Channel());
}

int Device::SetCardBaudrate(std::uint32_t baudrate){
    // ATRで指定されるB-CASの19200bpsとA-CASの38400bpsを受け付ける。
    if(baudrate!=19200 && baudrate!=38400)return -ENOTSUP;
    std::lock_guard<std::mutex> lock(card_mutex_);
    if(!card_open_ || !available_ || !receiver_)return -ENODEV;
    return card_.SetBaudrate(receiver_->Channel(),baudrate);
}

int Device::IsCardDataReady(bool &ready) try {
    std::lock_guard<std::mutex> lock(card_mutex_);
    ready=false;
    if(!card_open_ || !available_ || !receiver_)return -ENODEV;
    return card_.DataReady(receiver_->Channel(),ready);
} catch(...) {return -ENOMEM;}

int Device::ReadCardData(std::uint8_t *buf,std::uint8_t &len){
    std::lock_guard<std::mutex> lock(card_mutex_);
    if(!card_open_ || !available_)return -ENODEV;
    if(!buf || !len)return -EINVAL;
    len=static_cast<std::uint8_t>(card_.Read(buf,len));
    return 0;
}

int Device::WriteCardData(const std::uint8_t *buf,std::uint8_t len){
    std::lock_guard<std::mutex> lock(card_mutex_);
    if(!card_open_ || !available_ || !receiver_)return -ENODEV;
    return card_.Write(receiver_->Channel(),buf,len);
}
px4::ReceiverBase *Device::GetReceiver(int id) const {if(id)throw std::out_of_range("PIX-DT295 receiver index");return receiver_.get();}
}
