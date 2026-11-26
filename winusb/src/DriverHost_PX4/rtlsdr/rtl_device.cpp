// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "session.hpp"
#include "../stream_overflow.hpp"
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
class Receiver final: public ReceiverBase, private HelperPort {
public:
    Receiver(std::wstring serial,std::wstring device_path,const Config &config)
        :ReceiverBase(0),serial_(std::move(serial)),helper_(config.Get(L"HelperPath",L"rtlsdr\\rtlsdr_frontend.exe")),device_path_(std::move(device_path)),
         session_(*this,[this](const std::uint8_t *data,std::size_t size,std::uint64_t run){WriteTs(data,size,run);})
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
        session_.Stop();
    }
    void Disconnected() {available_=false;session_.Disconnect();stream_buf_->StopRequest();}
    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(open_) return -EBUSY;
        if(!available_) return -ECANCELED;
        if(GetFileAttributesW(helper_.c_str())==INVALID_FILE_ATTRIBUTES) {
            msg_err("RTL-SDR: helper is missing: %ls\n",helper_.c_str());return -ENOENT;
        }
        frequency_=0;open_=true;return 0;
    }
    void Close() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_) return;
        capture_=false;session_.Stop();stream_buf_->Stop();stream_started_=false;frequency_=0;
        open_=false;closed_.notify_all();
    }
    int CheckLock(bool &locked) override
    {
        locked=false;
        if(!available_)return -ECANCELED;
        if(!open_)return -EINVAL;
        if(session_.Exhausted())return -ECANCELED;
        locked=session_.Locked();
        return 0;
    }
    int SetLnbVoltage(std::int32_t voltage) override {return voltage ? -ENOSYS:0;}
    int SetCapture(bool capture) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !available_)return -ECANCELED;
        if(!capture){
            capture_=false;
            session_.Stop();
            // バッファを停止するとデータ接続の配信スレッドも終了する。
            return !stream_started_ || stream_buf_->Purge() ? 0 : -EIO;
        }
        if(capture_)return -EALREADY;
        if(!stream_started_){
            if(!stream_buf_->Alloc(188*8192))return -ENOMEM;
            stream_buf_->SetDiscardNullPacketsOnOutput(false);
            stream_buf_->SetThresholdSize(188*16);
            stream_buf_->Start();
            stream_started_=true;
        } else if(!stream_buf_->AdvanceEpoch()) {
            return -EIO;
        }
        capture_=true;
        const int result=frequency_?StartHelper():0;
        if(result)capture_=false;
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
        if(locked){const auto mer=session_.MerX100();value=mer>=0?mer*10:static_cast<std::int32_t>(session_.Quality()*10);}
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
        std::uint32_t hz=0;
        if(!open_ || !available_ || params_.system!=SystemType::ISDB_T || !HelperFrequency(params_.freq,hz))return -EINVAL;
        session_.Stop();
        // 旧 worker の書き込み停止後に世代を切り替え、前チャンネルの復調状態も破棄する。
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        frequency_=hz;
        return StartHelper();
    }
    int SetStreamId() override {return 0;}
private:
    int StartHelper()
    {
        int result=-EIO;
        try {result=session_.Start(frequency_);}
        catch(const std::exception &error){msg_err("RTL-SDR: launch failed: %s\n",error.what());session_.Stop();return -EIO;}
        if(!result)msg_info("RTL-SDR: started serial=%ls frequency=%u\n",serial_.c_str(),frequency_);
        return result;
    }
    // helper の TS を配信のバッファへ書く。Session の監視のスレッドから呼ばれる
    void WriteTs(const std::uint8_t *data,std::size_t requested,std::uint64_t run)
    {
        if(!capture_)return;
        // 満杯の記録は helper の起動ごとに分ける
        if(run!=overflow_run_){overflow_=px4::StreamOverflowLog("RTL-SDR");overflow_run_=run;}
        std::size_t size=requested;
        // 引数の評価順は決まっていないため、Write が size を書き換えてから記録へ渡す。
        const bool written=stream_buf_->Write(data,size);
        overflow_.Record(written,requested,size);
        stream_buf_->NotifyWrite();
    }
    // HelperPort: helper のプロセスとパイプを作る。Session のハンドルのロックを保持して呼ばれる。
    // 失敗した場合は作りかけのハンドルを閉じる
    int Launch(std::uint32_t hz,bool restarted) override
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
        suffix += L"-" + DeviceIdentity{device_path_, serial_}.PathId();
        // 起動し直した helper も同じ名前へ書くため、前の helper の終了理由を残すよう追記で開く
        const auto log=px4::util::path::GetDir()+L"\\rtlsdr_frontend-"+suffix+L".log";
        child_error.Set(CreateFileW(log.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,
            restarted ? OPEN_ALWAYS : CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
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
        std::wstring command=Quote(helper_)+L" --device-path "+Quote(device_path_)+L" --serial "+Quote(serial_)+L" --frequency "+std::to_wstring(hz)+L" --gain "+std::to_wstring(gain_);
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
    int Read(std::uint8_t *buffer,std::size_t size,std::chrono::milliseconds wait) override
    {
        DWORD available=0;
        if(!PeekNamedPipe(output_.Get(),nullptr,0,nullptr,&available,nullptr))return -EIO;
        if(!available){
            if(WaitForSingleObject(process_.Get(),0)!=WAIT_TIMEOUT)return -EPIPE;
            Sleep(static_cast<DWORD>(wait.count()));return 0;
        }
        DWORD got=0;
        if(!ReadFile(output_.Get(),buffer,(std::min)(available,static_cast<DWORD>(size)),&got,nullptr)||!got)return -EIO;
        return static_cast<int>(got);
    }
    // 停止の要求。監視のスレッドはこの後ハンドルを入れ替えないため、WaitStopped はロックの外で待てる
    void RequestStop() override {input_.Set(nullptr);stopping_process_=process_.Get();stopping_job_=job_.Get();}
    void WaitStopped(std::chrono::milliseconds limit) override
    {
        const HANDLE process=stopping_process_,job=stopping_job_;
        stopping_process_=nullptr;stopping_job_=nullptr;
        if(process && WaitForSingleObject(process,static_cast<DWORD>(limit.count()))==WAIT_TIMEOUT){
            msg_warn("RTL-SDR: helper stop deadline expired; terminating owned job\n");
            TerminateJobObject(job,ERROR_TIMEOUT);WaitForSingleObject(process,1000);
        }
    }
    // 形式不正や応答なしでは helper がまだ動いているため、job ごと止めてから回収する
    std::uint32_t Kill() override
    {
        DWORD exit_code=STILL_ACTIVE;
        if(job_)TerminateJobObject(job_.Get(),ERROR_INVALID_DATA);
        if(process_){WaitForSingleObject(process_.Get(),1000);GetExitCodeProcess(process_.Get(),&exit_code);}
        input_.Set(nullptr);output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);
        return exit_code;
    }
    void Release() override {output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);}
    /*
     * 機器のデバイスインターフェースがまだあるか。抜去では helper が USB の失敗で先に終わり、
     * DriverHost が抜去の通知を受け取るのはその後になることがある (MSi2500 の実機で確認)。
     * アクセス権を求めずに開くため、機器の状態は変えない
     */
    bool DevicePresent() override
    {
        const HANDLE h=CreateFileW(device_path_.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);return true;}
        const DWORD error=GetLastError();
        return error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND && error!=ERROR_NO_SUCH_DEVICE &&
            error!=ERROR_DEVICE_NOT_CONNECTED;
    }
    std::wstring serial_,helper_;
    // 起動し直す前に機器の有無を確かめるためのデバイスインターフェースのパス
    std::wstring device_path_;
    bool async_tune_=false;
    bool stream_started_=false; // mutex_ で保護し、Close まで配信バッファを維持する
    int gain_=297;
    std::uint32_t frequency_=0;
    std::mutex mutex_;std::condition_variable closed_;
    std::atomic_bool available_{true},open_{false},capture_{false};
    // helper のハンドル。Session のハンドルのロックで守る (HelperPort の契約)
    Handle process_,job_,input_,output_;
    // 停止を求めた helper。RequestStop で記録し、WaitStopped がロックの外で待つ
    HANDLE stopping_process_=nullptr,stopping_job_=nullptr;
    // 監視のスレッドだけが使う配信の満杯の記録と、記録を分ける helper の起動の番号
    px4::StreamOverflowLog overflow_{"RTL-SDR"};
    std::uint64_t overflow_run_=0;
    // 最後に置き、helper の監視のスレッドをほかのメンバーより先に止める
    Session session_;
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
