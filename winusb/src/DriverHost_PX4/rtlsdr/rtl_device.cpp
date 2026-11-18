// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "protocol.hpp"
#include "../stream_overflow.hpp"
#include "../pxbcud/recovery_budget.hpp"
#include "msg.h"
#include "util.hpp"
#include <array>
#include <condition_variable>
#include <thread>
#include <algorithm>

namespace px4::rtlsdr {
namespace {
class Handle {
public:
    Handle()=default;
    ~Handle(){Set(nullptr);}
    Handle(const Handle &)=delete;
    Handle &operator=(const Handle &)=delete;
    void Set(HANDLE value){if(value_ && value_!=INVALID_HANDLE_VALUE)CloseHandle(value_);value_=value;}
    HANDLE Get() const {return value_;}
    explicit operator bool() const {return value_ && value_!=INVALID_HANDLE_VALUE;}
private:
    HANDLE value_=nullptr;
};
std::wstring Quote(const std::wstring &input)
{
    std::wstring out=L"\"";
    std::size_t slashes=0;
    for(wchar_t c:input) {
        if(c==L'\\'){++slashes;continue;}
        if(c==L'"'){out.append(slashes*2+1,L'\\');out+=c;}
        else {out.append(slashes,L'\\');out+=c;}
        slashes=0;
    }
    out.append(slashes*2,L'\\');return out+L'"';
}
}
class Receiver final: public ReceiverBase {
public:
    Receiver(std::wstring serial,std::wstring device_path,const Config &config)
        :ReceiverBase(0),serial_(std::move(serial)),helper_(config.Get(L"HelperPath",L"rtlsdr\\rtlsdr_frontend.exe")),device_path_(std::move(device_path))
    {
        if(helper_.size()<2 || helper_[1]!=L':') helper_=px4::util::path::GetDir()+L"\\"+helper_;
        if(config.Exists(L"GainTenthsDb")) gain_=px4::util::wtoi(config.Get(L"GainTenthsDb"));
        async_tune_=px4::util::wtob(config.Get(L"AsyncTune",L"false"));
        if(gain_<-600 || gain_>600) throw DeviceError("RTL-SDR: invalid gain");
    }
    ~Receiver() override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 管理側の生ポインターを保持する接続が Close を完了するまで寿命を保つ。
        closed_.wait(lock,[this]{return !open_;});
        StopHelper();
    }
    void Disconnected() {available_=false;stop_=true;quality_=0;stream_buf_->StopRequest();}
    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(open_) return -EBUSY;
        if(!available_) return -ECANCELED;
        if(GetFileAttributesW(helper_.c_str())==INVALID_FILE_ATTRIBUTES) {
            msg_err("RTL-SDR: helper is missing: %ls\n",helper_.c_str());return -ENOENT;
        }
        frequency_=0;worker_error_=false;open_=true;return 0;
    }
    void Close() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_) return;
        capture_=false;StopHelper();stream_buf_->Stop();frequency_=0;
        open_=false;closed_.notify_all();
    }
    int CheckLock(bool &locked) override
    {
        locked=false;
        if(!available_)return -ECANCELED;
        if(!open_)return -EINVAL;
        if(worker_error_)return -ECANCELED;
        const auto last=last_ts_.load();
        locked=ready_ && last && GetTickCount64()-last<3000;
        return 0;
    }
    int SetLnbVoltage(std::int32_t voltage) override {return voltage ? -ENOSYS:0;}
    int SetCapture(bool capture) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !available_)return -ECANCELED;
        if(!capture){capture_=false;StopHelper();stream_buf_->Stop();return 0;}
        if(capture_)return -EALREADY;
        if(!stream_buf_->Alloc(188*8192))return -ENOMEM;
        stream_buf_->SetDiscardNullPacketsOnOutput(false);
        stream_buf_->SetThresholdSize(188*16);stream_buf_->Start();capture_=true;
        const int result=frequency_?StartHelper():0;
        if(result){capture_=false;stream_buf_->Stop();}
        return result;
    }
    int ReadStat(command::StatType type,std::int32_t &value) override
    {
        bool locked=false;const int result=CheckLock(locked);value=0;
        if(result)return result;
        if(type!=command::StatType::CNR)return -ENOSYS;
        // helper が報告する復調器の MER (dB×100) を、他の受信機の CNR と同じ dB×1000 で返す。
        // RS の成功割合は正常受信中ほぼ100%で変わらず、受信状態の目安にならないため。
        // MER をまだ受け取っていない間（TS 出力の直後、MER を送らない古い helper）は、従来どおり成功割合を返す。
        if(locked){const auto mer=mer_x100_.load();value=mer>=0?mer*10:static_cast<std::int32_t>(quality_.load()*10);}
        return 0;
    }
protected:
    int CheckTuneComplete(bool &complete) override
    {
        if(!async_tune_)return CheckLock(complete);
        std::lock_guard<std::mutex> lock(mutex_);
        complete=false;
        if(!available_)return -ECANCELED;
        if(!open_)return -EINVAL;
        // SetFrequency が成功した時点で選局は受付済み。helper の再起動中も受付結果は変えない。
        // USB 初期化・復調は非同期で、受信状態は CheckLock が返す。
        complete=true;
        return 0;
    }
    int SetFrequency() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !available_ || params_.system!=SystemType::ISDB_T)return -EINVAL;
        if(params_.freq<473143 || params_.freq>707143 || (params_.freq-473143)%6000)return -EINVAL;
        StopHelper();
        // 旧 worker の書き込み停止後に世代を切り替え、前チャンネルの復調状態も破棄する。
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        frequency_=473142857+((params_.freq-473143)/6000)*6000000;
        return StartHelper();
    }
    int SetStreamId() override {return 0;}
private:
    int StartHelper()
    {
        try {return Launch();}
        catch(const std::exception &error){msg_err("RTL-SDR: launch failed: %s\n",error.what());StopHelper();return -EIO;}
    }
    int Launch()
    {
        StopHelper();
        {
            std::lock_guard<std::mutex> handles(handle_mutex_);
            drain_only_=false;
            const int result=StartProcess();
            if(result){drain_only_=true;return result;}
        }
        stop_=false;worker_error_=false;ready_=false;last_ts_=0;quality_=0;mer_x100_=-1;
        worker_=std::thread([this]{Supervise();});
        msg_info("RTL-SDR: started serial=%ls frequency=%u\n",serial_.c_str(),frequency_);
        return 0;
    }
    // helper のプロセスとパイプを作る。handle_mutex_ を保持して呼ぶ。失敗した場合は作りかけのハンドルを閉じる
    int StartProcess()
    {
        auto fail=[this](int result){input_.Set(nullptr);output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);return result;};
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        HANDLE a=nullptr,b=nullptr;
        Handle child_input,child_output,child_error;
        if(!CreatePipe(&a,&b,&sa,0))return fail(-EIO);
        child_input.Set(a);input_.Set(b);
        if(!SetHandleInformation(input_.Get(),HANDLE_FLAG_INHERIT,0))return fail(-EIO);
        if(!CreatePipe(&a,&b,&sa,0))return fail(-EIO);
        output_.Set(a);child_output.Set(b);
        if(!SetHandleInformation(output_.Get(),HANDLE_FLAG_INHERIT,0))return fail(-EIO);
        std::wstring suffix=serial_;
        for(auto &c:suffix)if(!((c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z')))c=L'_';
        // 起動し直した helper も同じ名前へ書くため、前の helper の終了理由を残すよう追記で開く
        const auto log=px4::util::path::GetDir()+L"\\rtlsdr_frontend-"+suffix+L".log";
        child_error.Set(CreateFileW(log.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,
            restart_count_ ? OPEN_ALWAYS : CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!child_error)return fail(-EIO);
        job_.Set(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job_ || !SetInformationJobObject(job_.Get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return fail(-EIO);
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=child_input.Get();startup.StartupInfo.hStdOutput=child_output.Get();startup.StartupInfo.hStdError=child_error.Get();
        SIZE_T attribute_size=0;
        InitializeProcThreadAttributeList(nullptr,1,0,&attribute_size);
        std::vector<std::uint8_t> attributes(attribute_size);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size))return fail(-EIO);
        HANDLE inherited[]={child_input.Get(),child_output.Get(),child_error.Get()};
        const bool attr_ok=UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE;
        std::wstring command=Quote(helper_)+L" --serial "+Quote(serial_)+L" --frequency "+std::to_wstring(frequency_)+L" --gain "+std::to_wstring(gain_);
        PROCESS_INFORMATION process{};
        const BOOL created=attr_ok && CreateProcessW(helper_.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if(!created){msg_err("RTL-SDR: CreateProcess failed win32=%lu\n",GetLastError());return fail(-EIO);}
        process_.Set(process.hProcess);Handle thread;thread.Set(process.hThread);
        if(!AssignProcessToJobObject(job_.Get(),process_.Get())){
            TerminateProcess(process_.Get(),ERROR_ACCESS_DENIED);WaitForSingleObject(process_.Get(),1000);return fail(-EIO);
        }
        if(ResumeThread(thread.Get())==static_cast<DWORD>(-1)){TerminateJobObject(job_.Get(),ERROR_INVALID_FUNCTION);return fail(-EIO);}
        // 子への出力書込端を親が残すと、子の終了を EOF として検出できなくなる。
        child_input.Set(nullptr);child_output.Set(nullptr);child_error.Set(nullptr);
        return 0;
    }
    void StopHelper()
    {
        // 停止要求後も stdout を排出する。先に reader を止めると、パイプへ書き込み中の
        // helper が詰まり、stdin の停止要求を受けても終了できなくなる。
        // drain_only_ を立てた後は監視のスレッドがハンドルを入れ替えないため、ロックの外で待てる
        HANDLE process=nullptr,job=nullptr;
        {
            std::lock_guard<std::mutex> handles(handle_mutex_);
            drain_only_=true;input_.Set(nullptr);
            process=process_.Get();job=job_.Get();
        }
        if(process && WaitForSingleObject(process,3000)==WAIT_TIMEOUT){
            msg_warn("RTL-SDR: helper stop deadline expired; terminating owned job\n");
            TerminateJobObject(job,ERROR_TIMEOUT);WaitForSingleObject(process,1000);
        }
        stop_=true;
        if(worker_.joinable())worker_.join();
        {
            std::lock_guard<std::mutex> handles(handle_mutex_);
            output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);
        }
        ready_=false;last_ts_=0;quality_=0;mer_x100_=-1;
    }
    // 起動し直しを止めるべきか。Close・選局・取り込みの停止 (drain_only_) と抜去 (available_) を優先する
    bool Stopping() const {return stop_ || drain_only_ || !available_;}
    /*
     * 機器のデバイスインターフェースがまだあるか。抜去では helper が USB の失敗で先に終わり、
     * DriverHost が抜去の通知を受け取るのはその後になることがある (MSi2500 の実機で確認)。
     * 通知の前に起動し直すと、抜いた機器へ helper を起動して失敗させるため、パスが消えていれば待つ。
     * アクセス権を求めずに開くため、機器の状態は変えない
     */
    bool DevicePresent() const
    {
        const HANDLE h=CreateFileW(device_path_.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);return true;}
        const DWORD error=GetLastError();
        return error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND && error!=ERROR_NO_SUCH_DEVICE &&
            error!=ERROR_DEVICE_NOT_CONNECTED;
    }
    /*
     * helper が予期せず終わったとき (USB の I/Q が止まる、異常終了、応答なし、形式不正)、同じ周波数で helper を
     * 起動し直す。配信の接続と世代は保ち、欠けた区間は配信先が continuity counter で検出する。打ち切ると
     * 選局し直すまで録画が止まるため、PX-BCUD の自動復旧と同じ回数の制限と待機で続ける。
     * この helper はどの失敗でも終了コード 1 を返す。引数は DriverHost が起動前に確かめているため、
     * 終了コードでは区別せず、回数の制限で止める。回数を使い切った後も配信の接続は残し、
     * CheckLock の失敗で知らせて選局し直しを待つ。
     */
    void Supervise() noexcept
    {
        px4::pxbcud::RecoveryBudget budget;
        for(;;) {
            const auto run=ReadHelper();
            if(Stopping())break;
            DWORD exit_code=STILL_ACTIVE;
            {
                std::lock_guard<std::mutex> handles(handle_mutex_);
                if(Stopping())break;
                // 形式不正や応答なしでは helper がまだ動いているため、job ごと止めてから回収する
                if(job_)TerminateJobObject(job_.Get(),ERROR_INVALID_DATA);
                if(process_){WaitForSingleObject(process_.Get(),1000);GetExitCodeProcess(process_.Get(),&exit_code);}
                input_.Set(nullptr);output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);
            }
            ready_=false;quality_=0;mer_x100_=-1;
            const auto failed_at=GetTickCount64();
            budget.NoteStream(std::chrono::milliseconds(run.received_ms));
            bool started=false;
            while(!started) {
                if(!budget.Take()){
                    Exhausted(exit_code);
                    return;
                }
                const auto backoff=budget.Backoff();
                for(auto waited=std::chrono::milliseconds::zero();waited<backoff && !Stopping();waited+=std::chrono::milliseconds(10))Sleep(10);
                // パスが消えていれば抜去の通知を最長 3 秒待つ。期限を過ぎてもないときは起動し直しの失敗として数える
                if(!DevicePresent()){
                    for(int i=0;i<300 && !Stopping() && !DevicePresent();++i)Sleep(10);
                    if(Stopping())return;
                    if(!DevicePresent()){msg_warn("RTL-SDR: device interface is missing; not restarting the helper\n");continue;}
                }
                std::lock_guard<std::mutex> handles(handle_mutex_);
                if(Stopping())return;
                ++restart_count_;
                msg_warn("RTL-SDR: helper restart count=%llu exit_code=%lu consecutive=%u backoff_ms=%lld possible_ts_loss=1\n",
                    static_cast<unsigned long long>(restart_count_),exit_code,static_cast<unsigned>(budget.Consecutive()),
                    static_cast<long long>(backoff.count()));
                started=!StartProcess();
                if(!started)msg_err("RTL-SDR: helper restart failed count=%llu\n",static_cast<unsigned long long>(restart_count_));
            }
            restarted_at_=failed_at;
        }
    }
    void Exhausted(DWORD exit_code)
    {
        worker_error_=true;quality_=0;
        msg_err("RTL-SDR: auto restart exhausted exit_code=%lu restart_count=%llu limit=%u; waiting for a retune\n",exit_code,
            static_cast<unsigned long long>(restart_count_),static_cast<unsigned>(px4::pxbcud::RecoveryBudget::Limit));
    }
    struct HelperRun {std::uint64_t received_ms=0;};
    // 1 つの helper の出力を、終わるか停止を求められるまで読む。最初と最後の TS の間を返す
    HelperRun ReadHelper() noexcept
    {
        HelperRun run;
        try {
            Frames frames;std::array<std::uint8_t,16384> bytes{};
            px4::StreamOverflowLog overflow("RTL-SDR");
            auto last_message=GetTickCount64();
            ULONGLONG first_ts=0;
            while(!stop_ && available_) {
                DWORD available=0;
                if(!PeekNamedPipe(output_.Get(),nullptr,0,nullptr,&available,nullptr))break;
                if(!available){
                    if(WaitForSingleObject(process_.Get(),0)!=WAIT_TIMEOUT)break;
                    if(GetTickCount64()-last_message>30000){msg_err("RTL-SDR: helper heartbeat stalled\n");break;}
                    Sleep(10);continue;
                }
                DWORD got=0;
                if(!ReadFile(output_.Get(),bytes.data(),(std::min)(available,static_cast<DWORD>(bytes.size())),&got,nullptr)||!got)break;
                const bool valid=frames.Feed(bytes.data(),got,[&](const Frame &frame,const std::uint8_t *data){
                    last_message=GetTickCount64();
                    if(frame.kind==1)ready_=true;
                    if(frame.kind==4){msg_err("RTL-SDR: helper error: %.*s\n",static_cast<int>(frame.size),reinterpret_cast<const char *>(data));return false;}
                    if(frame.kind==3){
                        quality_=frame.quality;
                        // MER は TS 出力中だけ載る。載らない状態フレームでは直前の値を残し、ロックの判定は TS の到着で行う。
                        std::uint32_t mer=0;
                        if(StatusMer(frame,data,mer))mer_x100_=static_cast<std::int32_t>(mer);
                    }
                    if(frame.kind==2){
                        quality_=frame.quality;last_ts_=GetTickCount64();
                        if(!first_ts)first_ts=last_ts_;
                        run.received_ms=last_ts_-first_ts;
                        if(const auto restarted=restarted_at_.exchange(0)){
                            msg_warn("RTL-SDR: helper restart resumed count=%llu outage_ms=%llu possible_ts_loss=1\n",
                                static_cast<unsigned long long>(restart_count_),static_cast<unsigned long long>(last_ts_-restarted));
                        }
                        if(capture_ && !stop_ && !drain_only_){
                            std::size_t size=frame.size;
                            // 引数の評価順は決まっていないため、Write が size を書き換えてから記録へ渡す。
                            const bool written=stream_buf_->Write(data,size);
                            overflow.Record(written,frame.size,size);
                            stream_buf_->NotifyWrite();
                        }
                    }
                    return true;
                });
                if(!valid){msg_err("RTL-SDR: invalid or failed helper frame\n");break;}
            }
        } catch(const std::exception &error){msg_err("RTL-SDR: reader failed: %s\n",error.what());}
        if(!Stopping())msg_err("RTL-SDR: frontend stopped unexpectedly\n");
        return run;
    }
    std::wstring serial_,helper_;
    // 起動し直す前に機器の有無を確かめるためのデバイスインターフェースのパス
    std::wstring device_path_;
    bool async_tune_=false;
    int gain_=297;
    std::uint32_t frequency_=0;
    std::mutex mutex_;std::condition_variable closed_;
    std::atomic_bool available_{true},open_{false},capture_{false},stop_{true},drain_only_{true},worker_error_{false},ready_{false};
    std::atomic<std::uint32_t> quality_{0};
    std::atomic<std::int32_t> mer_x100_{-1};
    std::atomic<ULONGLONG> last_ts_{0};
    // helper のハンドル。監視のスレッドが起動し直すときに入れ替えるため、handle_mutex_ で守る。
    // 取得順は mutex_、handle_mutex_ の順で、監視のスレッドは handle_mutex_ だけを取る
    std::mutex handle_mutex_;
    Handle process_,job_,input_,output_;
    // 起動し直した回数 (handle_mutex_ を保持して更新する) と、起動し直した後に最初の TS を待つ間の失敗の時刻
    std::uint64_t restart_count_=0;
    std::atomic<ULONGLONG> restarted_at_{0};
    std::thread worker_;
};

Device::Device(const std::wstring &path,const DeviceDefinition &definition,std::uintptr_t index,ReceiverManager &manager)
    :DeviceBase(path,definition,index,manager,true)
{
    // 記述子・個体情報の取得後は helper だけが USB を開く。既存機種の所有方式は維持する。
    WinUsb_Free(usb_dev_.winusb);usb_dev_.winusb=nullptr;
    CloseHandle(usb_dev_.dev);usb_dev_.dev=INVALID_HANDLE_VALUE;
}
Device::~Device(){SetAvailability(false);Term();}
int Device::Init()
{
    if(!available_)return -ECANCELED;
    if(receiver_)return -EALREADY;
    if(usb_dev_.descriptor.idVendor!=0x0bda || usb_dev_.descriptor.idProduct!=0x2838 || usb_serial_number_.empty())return -ENODEV;
    if(device_def_.receivers.size()!=1 || device_def_.receivers[0].systems!=SystemType::ISDB_T)return -EINVAL;
    receiver_=std::make_unique<Receiver>(usb_serial_number_,device_path_,device_def_.configs);
    const auto &definition=device_def_.receivers[0];command::ReceiverInfo info{};
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,definition.name.c_str());info.receiver_guid=definition.guid;
    info.systems=SystemType::ISDB_T;info.index=definition.index;
    if(!receiver_manager_.Register(info,receiver_.get(),GetDeviceIdentity())){receiver_.reset();return -EIO;}
    return 0;
}
void Device::Term(){SetAvailability(false);if(receiver_){receiver_manager_.Unregister(receiver_.get());receiver_.reset();}}
void Device::SetAvailability(bool available){available_=available;if(!available && receiver_)receiver_->Disconnected();}
ReceiverBase *Device::GetReceiver(int id) const {if(id!=0)throw std::out_of_range("RTL-SDR receiver index");return receiver_.get();}
}
