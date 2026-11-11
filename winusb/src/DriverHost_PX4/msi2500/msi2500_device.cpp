// SPDX-License-Identifier: GPL-2.0-only
// RSP1 互換機 (Mirics MSi2500 + MSi001) によるフルセグ受信。
// helper の起動・停止と標準出力のフレーム形式は RTL-SDR の helper (rtlsdr/rtl_device.cpp) と同じで、
// フレームの解釈は rtlsdr/protocol.hpp を共用する。RTL-SDR の実装を変えずに追加するため、
// 受信機クラスは機種ごとに持つ。両機種を実機で確認した後に共通化を検討する。
#include "device.hpp"
#include "../rtlsdr/protocol.hpp"
#include "msg.h"
#include "util.hpp"
#include <array>
#include <condition_variable>
#include <thread>
#include <algorithm>

namespace px4::msi2500 {
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
using px4::rtlsdr::Frame;
using px4::rtlsdr::Frames;

class Receiver final: public ReceiverBase {
public:
    Receiver(std::wstring device_path,std::uintptr_t index,const Config &config,bool j83b)
        :ReceiverBase(0),device_path_(std::move(device_path)),index_(index),helper_(config.Get(L"HelperPath",L"msi2500\\msi2500_frontend.exe")),j83b_(j83b)
    {
        if(helper_.size()<2 || helper_[1]!=L':') helper_=px4::util::path::GetDir()+L"\\"+helper_;
        // 利得は MSi001 の LNA・ミキサー・IF の合計 (dB)。helper の --gain と同じ 0〜102
        if(config.Exists(L"GainDb")) gain_=px4::util::wtoi(config.Get(L"GainDb"));
        // J.83B はケーブルの入力レベルが地上波のアンテナ入力と異なるため、別の値を持つ。
        // 検証環境では 58 で ADC が飽和して RS の訂正が急増し、46〜52 で誤りがなかった
        if(config.Exists(L"GainDbJ83B")) gain_j83b_=px4::util::wtoi(config.Get(L"GainDbJ83B"));
        if(config.Exists(L"ViterbiThreads")) viterbi_threads_=px4::util::wtoi(config.Get(L"ViterbiThreads"));
        async_tune_=px4::util::wtob(config.Get(L"AsyncTune",L"false"));
        if(gain_<0 || gain_>102 || gain_j83b_<0 || gain_j83b_>102) throw DeviceError("MSi2500: invalid gain");
        if(viterbi_threads_<0 || viterbi_threads_>64) throw DeviceError("MSi2500: invalid Viterbi thread count");
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
            msg_err("MSi2500: helper is missing: %ls\n",helper_.c_str());return -ENOENT;
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
        // フルセグの TS は約 2.1 MB/s で、helper の出力は再多重化で約 1 フレーム (0.23 秒) ごとにまとまって届く。
        // J.83B (256QAM、5.6 Msps) の TS は約 4.85 MB/s。選局の前に確保するため、J.83B でも約 3 秒分になる大きさを
        // どちらの系統にも確保する (フルセグでは約 7 秒分)
        if(!stream_buf_->Alloc(188*81920))return -ENOMEM;
        stream_buf_->SetDiscardNullPacketsOnOutput(false);
        stream_buf_->SetThresholdSize(188*64);stream_buf_->Start();capture_=true;
        const int result=frequency_?StartHelper():0;
        if(result){capture_=false;stream_buf_->Stop();}
        return result;
    }
    int ReadStat(command::StatType type,std::int32_t &value) override
    {
        bool locked=false;const int result=CheckLock(locked);value=0;
        if(result)return result;
        if(type!=command::StatType::CNR)return -ENOSYS;
        // RTL-SDR と同じ 0〜100 の品質点 (ISDB-T は B 階層、J.83B は全ブロックの RS 復号の成功割合)。校正した RF C/N ではない。
        if(locked)value=static_cast<std::int32_t>(quality_.load()*10);
        return 0;
    }
protected:
    int CheckTuneComplete(bool &complete) override
    {
        if(!async_tune_)return CheckLock(complete);
        std::lock_guard<std::mutex> lock(mutex_);
        complete=false;
        if(!available_ || worker_error_)return -ECANCELED;
        if(!open_)return -EINVAL;
        if(!process_ || WaitForSingleObject(process_.Get(),0)!=WAIT_TIMEOUT)return -ECANCELED;
        // 子プロセスへ選局要求を渡した時点で返す。USB の初期化と復調は非同期で、
        // CheckLock と信号品質は実際の TS 出力まで未ロックのまま。
        complete=true;
        return 0;
    }
    int SetFrequency() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!open_ || !available_)return -EINVAL;
        std::uint32_t frequency=0;
        if(params_.system==SystemType::ISDB_T) {
            if(params_.freq<473143 || params_.freq>707143 || (params_.freq-473143)%6000)return -EINVAL;
            frequency=473142857+((params_.freq-473143)/6000)*6000000;
        } else if(params_.system==SystemType::J83B && j83b_) {
            // J.83B はチャンネル定義の中心周波数 (kHz) をそのまま使う。地域によって H001〜H105 からずれた周波数があるため、
            // 6.5 MHz 刻みに限らず helper の受け付ける範囲 (90〜775 MHz) で受ける
            if(params_.freq<90000 || params_.freq>775000)return -EINVAL;
            frequency=params_.freq*1000;
        } else return -EINVAL;
        StopHelper();
        // 旧 helper の書き込み停止後に世代を切り替え、前チャンネルの復調状態も破棄する。
        if(!stream_buf_->AdvanceEpoch())return -EIO;
        frequency_=frequency;system_j83b_=params_.system==SystemType::J83B;
        return StartHelper();
    }
    int SetStreamId() override {return 0;}
private:
    int StartHelper()
    {
        try {return Launch();}
        catch(const std::exception &error){msg_err("MSi2500: launch failed: %s\n",error.what());StopHelper();return -EIO;}
    }
    int Launch()
    {
        StopHelper();
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        HANDLE a=nullptr,b=nullptr;
        Handle child_input,child_output,child_error;
        if(!CreatePipe(&a,&b,&sa,0))return -EIO;
        child_input.Set(a);input_.Set(b);
        if(!SetHandleInformation(input_.Get(),HANDLE_FLAG_INHERIT,0)){StopHelper();return -EIO;}
        // helper はフルセグの TS を約 1 フレームごとにまとめて書くため、パイプの容量を大きく取る
        if(!CreatePipe(&a,&b,&sa,1<<20)){StopHelper();return -EIO;}
        output_.Set(a);child_output.Set(b);
        if(!SetHandleInformation(output_.Get(),HANDLE_FLAG_INHERIT,0)){StopHelper();return -EIO;}
        // この機器はシリアル番号を返さない個体があるため、ログ名は DriverHost 内の機器番号で区別する
        const auto log=px4::util::path::GetDir()+L"\\msi2500_frontend-"+std::to_wstring(index_)+L".log";
        child_error.Set(CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!child_error){StopHelper();return -EIO;}
        job_.Set(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job_ || !SetInformationJobObject(job_.Get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))){StopHelper();return -EIO;}
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=child_input.Get();startup.StartupInfo.hStdOutput=child_output.Get();startup.StartupInfo.hStdError=child_error.Get();
        SIZE_T attribute_size=0;
        InitializeProcThreadAttributeList(nullptr,1,0,&attribute_size);
        std::vector<std::uint8_t> attributes(attribute_size);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size)){StopHelper();return -EIO;}
        HANDLE inherited[]={child_input.Get(),child_output.Get(),child_error.Get()};
        const bool attr_ok=UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE;
        // 個体は DriverHost が列挙したデバイスインターフェースのパスで指定する
        std::wstring command=Quote(helper_)+L" serve --path "+Quote(device_path_)+L" --frequency "+std::to_wstring(frequency_)
            +L" --gain "+std::to_wstring(system_j83b_ ? gain_j83b_ : gain_);
        if(system_j83b_)command+=L" --system j83b";
        else if(viterbi_threads_)command+=L" --viterbi-threads "+std::to_wstring(viterbi_threads_);
        PROCESS_INFORMATION process{};
        const BOOL created=attr_ok && CreateProcessW(helper_.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if(!created){msg_err("MSi2500: CreateProcess failed win32=%lu\n",GetLastError());StopHelper();return -EIO;}
        process_.Set(process.hProcess);Handle thread;thread.Set(process.hThread);
        if(!AssignProcessToJobObject(job_.Get(),process_.Get())){
            TerminateProcess(process_.Get(),ERROR_ACCESS_DENIED);WaitForSingleObject(process_.Get(),1000);StopHelper();return -EIO;
        }
        stop_=false;drain_only_=false;worker_error_=false;ready_=false;last_ts_=0;quality_=0;
        if(ResumeThread(thread.Get())==static_cast<DWORD>(-1)){StopHelper();return -EIO;}
        // 子への出力書込端を親が残すと、子の終了を EOF として検出できなくなる。
        child_input.Set(nullptr);child_output.Set(nullptr);child_error.Set(nullptr);
        worker_=std::thread([this]{ReadHelper();});
        msg_info("MSi2500: started device=%llu system=%s frequency=%u\n",static_cast<unsigned long long>(index_),system_j83b_ ? "J.83B" : "ISDB-T",frequency_);
        return 0;
    }
    void StopHelper()
    {
        // 停止要求後も stdout を排出する。先に reader を止めると、パイプへ書き込み中の
        // helper が詰まり、stdin の停止要求を受けても終了できなくなる。
        // helper は停止要求の後に USB の転送を止め、停止要求と ADC の休止を送ってから終わる。
        // この順序を省くと次回の取り込みでデータが届かない状態に入ることがあったため、強制終了までの期限は長めに取る
        drain_only_=true;input_.Set(nullptr);
        if(process_ && WaitForSingleObject(process_.Get(),5000)==WAIT_TIMEOUT){
            msg_warn("MSi2500: helper stop deadline expired; terminating owned job\n");
            TerminateJobObject(job_.Get(),ERROR_TIMEOUT);WaitForSingleObject(process_.Get(),1000);
        }
        stop_=true;
        if(worker_.joinable())worker_.join();
        output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);
        ready_=false;last_ts_=0;quality_=0;
    }
    void ReadHelper() noexcept
    {
        try {
            Frames frames;std::array<std::uint8_t,65536> bytes{};
            auto last_message=GetTickCount64();
            while(!stop_ && available_) {
                DWORD available=0;
                if(!PeekNamedPipe(output_.Get(),nullptr,0,nullptr,&available,nullptr))break;
                if(!available){
                    if(WaitForSingleObject(process_.Get(),0)!=WAIT_TIMEOUT)break;
                    if(GetTickCount64()-last_message>30000){msg_err("MSi2500: helper heartbeat stalled\n");break;}
                    Sleep(10);continue;
                }
                DWORD got=0;
                if(!ReadFile(output_.Get(),bytes.data(),(std::min)(available,static_cast<DWORD>(bytes.size())),&got,nullptr)||!got)break;
                // Frames::Feed は 1 回の入力を MaxTs までに制限するため、分けて渡す
                bool valid=true;
                for(DWORD offset=0;valid && offset<got;) {
                    const DWORD part=(std::min)(got-offset,static_cast<DWORD>(px4::rtlsdr::MaxTs));
                    valid=frames.Feed(bytes.data()+offset,part,[&](const Frame &frame,const std::uint8_t *data){
                        last_message=GetTickCount64();
                        if(frame.kind==1)ready_=true;
                        if(frame.kind==4){msg_err("MSi2500: helper error: %.*s\n",static_cast<int>(frame.size),reinterpret_cast<const char *>(data));return false;}
                        if(frame.kind==3)quality_=frame.quality;
                        if(frame.kind==2){
                            quality_=frame.quality;last_ts_=GetTickCount64();
                            if(capture_ && !stop_ && !drain_only_){
                                std::size_t size=frame.size;
                                if(!stream_buf_->Write(data,size)||size!=frame.size){msg_err("MSi2500: TS ring overflow\n");return false;}
                                stream_buf_->NotifyWrite();
                            }
                        }
                        return true;
                    });
                    offset+=part;
                }
                if(!valid){msg_err("MSi2500: invalid or failed helper frame\n");break;}
            }
        } catch(const std::exception &error){msg_err("MSi2500: reader failed: %s\n",error.what());}
        if(!stop_ && !drain_only_ && available_){worker_error_=true;quality_=0;stream_buf_->StopRequest();TerminateJobObject(job_.Get(),ERROR_INVALID_DATA);msg_err("MSi2500: frontend stopped unexpectedly\n");}
    }
    std::wstring device_path_;
    std::uintptr_t index_;
    std::wstring helper_;
    bool async_tune_=false;
    int gain_=40;
    int gain_j83b_=46;
    // 受信機定義で J.83B を受け付けるか (Device::Init で設定する) と、起動中の helper の系統
    bool j83b_=false;
    bool system_j83b_=false;
    int viterbi_threads_=0;
    std::uint32_t frequency_=0;
    std::mutex mutex_;std::condition_variable closed_;
    std::atomic_bool available_{true},open_{false},capture_{false},stop_{true},drain_only_{true},worker_error_{false},ready_{false};
    std::atomic<std::uint32_t> quality_{0};
    std::atomic<ULONGLONG> last_ts_{0};
    Handle process_,job_,input_,output_;
    std::thread worker_;
};

Device::Device(const std::wstring &path,const DeviceDefinition &definition,std::uintptr_t index,ReceiverManager &manager)
    :DeviceBase(path,definition,index,manager,true),index_(index)
{
    // 記述子の取得後は helper だけが USB を開く。
    WinUsb_Free(usb_dev_.winusb);usb_dev_.winusb=nullptr;
    CloseHandle(usb_dev_.dev);usb_dev_.dev=INVALID_HANDLE_VALUE;
}
Device::~Device(){SetAvailability(false);Term();}
int Device::Init()
{
    if(!available_)return -ECANCELED;
    if(receiver_)return -EALREADY;
    // Mirics MSi2500 (SDRplay RSP1 と互換機)。シリアル番号は空の個体があるため条件に含めない
    if(usb_dev_.descriptor.idVendor!=0x1df7 || usb_dev_.descriptor.idProduct!=0x2500)return -ENODEV;
    // 受信機は 1 つで、ISDB-T を必須とし、J.83B は ini の System に書いた場合だけ受け付ける
    if(device_def_.receivers.size()!=1)return -EINVAL;
    const auto systems=device_def_.receivers[0].systems;
    constexpr auto supported=SystemType::ISDB_T|SystemType::J83B;
    if((systems&SystemType::ISDB_T)!=SystemType::ISDB_T || (systems|supported)!=supported)return -EINVAL;
    receiver_=std::make_unique<Receiver>(device_path_,index_,device_def_.configs,(systems&SystemType::J83B)==SystemType::J83B);
    const auto &definition=device_def_.receivers[0];command::ReceiverInfo info{};
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,definition.name.c_str());info.receiver_guid=definition.guid;
    info.systems=systems;info.index=definition.index;
    if(!receiver_manager_.Register(info,receiver_.get(),GetDeviceIdentity())){receiver_.reset();return -EIO;}
    return 0;
}
void Device::Term(){SetAvailability(false);if(receiver_){receiver_manager_.Unregister(receiver_.get());receiver_.reset();}}
void Device::SetAvailability(bool available){available_=available;if(!available && receiver_)receiver_->Disconnected();}
ReceiverBase *Device::GetReceiver(int id) const {if(id!=0)throw std::out_of_range("MSi2500 receiver index");return receiver_.get();}
}
