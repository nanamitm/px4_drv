// SPDX-License-Identifier: GPL-2.0-only
// RSP1 互換機 (Mirics MSi2500 + MSi001) によるフルセグ受信。
// helper の起動・停止と標準出力のフレーム形式は RTL-SDR の helper (rtlsdr/rtl_device.cpp) と同じで、
// フレームの解釈は rtlsdr/protocol.hpp を共用する。RTL-SDR の実装を変えずに追加するため、
// 受信機クラスは機種ごとに持つ。両機種を実機で確認した後に共通化を検討する。
#include "device.hpp"
#include "../rtlsdr/protocol.hpp"
#include "../stream_overflow.hpp"
#include "../pxbcud/recovery_budget.hpp"
#include "../config_int.hpp"
#include "msg.h"
#include "util.hpp"
#include <array>
#include <condition_variable>
#include <thread>
#include <algorithm>
#include <filesystem>

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
        // 相対パスだけを DriverHost のディレクトリから解決する。2 文字目の ':' だけで判定すると、UNC パス (\\server\share\...)
        // の前にもディレクトリを付けてしまうため、PX-S1UD の HelperPath と同じく std::filesystem で判定する
        if(const std::filesystem::path path=helper_;path.is_relative())
            helper_=(std::filesystem::path(px4::util::path::GetDir())/path).lexically_normal().wstring();
        // 利得は MSi001 の LNA・ミキサー・IF の合計 (dB)。helper の --gain と同じ 0〜102
        gain_=ConfigInt(config,L"GainDb",gain_,0,102,"MSi2500");
        // J.83B はケーブルの入力レベルが地上波のアンテナ入力と異なるため、別の値を持つ。
        // 検証環境では 58 で ADC が飽和して RS の訂正が急増し、46〜52 で誤りがなかった
        gain_j83b_=ConfigInt(config,L"GainDbJ83B",gain_j83b_,0,102,"MSi2500");
        viterbi_threads_=ConfigInt(config,L"ViterbiThreads",viterbi_threads_,0,64,"MSi2500");
        async_tune_=px4::util::wtob(config.Get(L"AsyncTune",L"false"));
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
        capture_=false;StopHelper();stream_buf_->Stop();stream_started_=false;frequency_=0;
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
        if(!capture){
            capture_=false;
            StopHelper();
            // 配信スレッドは同じデータ接続での再開まで待機させる。
            return !stream_started_ || stream_buf_->Purge() ? 0 : -EIO;
        }
        if(capture_)return -EALREADY;
        if(!stream_started_){
            // フルセグは約 2.1 MB/s、J.83B は約 4.85 MB/s。どちらにも J.83B の約 3 秒分を確保する。
            if(!stream_buf_->Alloc(188*81920))return -ENOMEM;
            stream_buf_->SetDiscardNullPacketsOnOutput(false);
            stream_buf_->SetThresholdSize(188*64);
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
        // helper が報告する復調器の MER (dB×100、ISDB-T は B 階層、J.83B は判定帰還中の復調器) を、
        // 他の受信機の CNR と同じ dB×1000 で返す。RS の成功割合は正常受信中ほぼ 100% で変わらず、受信状態の目安にならないため。
        // MER をまだ受け取っていない間 (TS 出力の直後、MER を送らない古い helper) は、成功割合 (0〜100 の品質点) を返す。
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
        // SetFrequency の成功で受付済み。再起動中も受付結果は変えず、受信状態は CheckLock が返す。
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
    // helper の終了コード。引数の誤りは起動し直しても直らないため、自動の起動し直しの対象外にする
    static constexpr DWORD HelperArgumentError=2;
    int StartHelper()
    {
        try {return Launch();}
        catch(const std::exception &error){msg_err("MSi2500: launch failed: %s\n",error.what());StopHelper();return -EIO;}
    }
    int Launch()
    {
        StopHelper();
        {
            std::lock_guard<std::mutex> handles(handle_mutex_);
            drain_only_=false;
            const int result=StartProcess(false);
            if(result){drain_only_=true;return result;}
        }
        stop_=false;worker_error_=false;ready_=false;last_ts_=0;quality_=0;mer_x100_=-1;
        worker_=std::thread([this]{Supervise();});
        msg_info("MSi2500: started device=%llu system=%s frequency=%u\n",static_cast<unsigned long long>(index_),system_j83b_ ? "J.83B" : "ISDB-T",frequency_);
        return 0;
    }
    // helper のプロセスとパイプを作る。handle_mutex_ を保持して呼ぶ。失敗した場合は作りかけのハンドルを閉じる。
    // restart は監視のスレッドによる自動の起動し直しか (ログを追記で開く)
    int StartProcess(bool restart)
    {
        auto fail=[this](int result){input_.Set(nullptr);output_.Set(nullptr);process_.Set(nullptr);job_.Set(nullptr);return result;};
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        HANDLE a=nullptr,b=nullptr;
        Handle child_input,child_output,child_error;
        if(!CreatePipe(&a,&b,&sa,0))return fail(-EIO);
        child_input.Set(a);input_.Set(b);
        if(!SetHandleInformation(input_.Get(),HANDLE_FLAG_INHERIT,0))return fail(-EIO);
        // helper はフルセグの TS を約 1 フレームごとにまとめて書くため、パイプの容量を大きく取る
        if(!CreatePipe(&a,&b,&sa,1<<20))return fail(-EIO);
        output_.Set(a);child_output.Set(b);
        if(!SetHandleInformation(output_.Get(),HANDLE_FLAG_INHERIT,0))return fail(-EIO);
        // この機器はシリアル番号を返さない個体があるため、ログ名は DriverHost 内の機器番号で区別する。
        // 起動し直した helper も同じ名前へ書くため、前の helper の終了理由を残すよう追記で開く。
        // 選局と取り込みの開始では作り直す。機器の寿命で判定すると、一度でも起動し直した後は数日の録画の間すべての
        // 選局の出力が追記され続けるため、追記は同じ選局の中の起動し直しに限る
        const auto log=px4::util::path::GetDir()+L"\\msi2500_frontend-"+std::to_wstring(index_)+L".log";
        child_error.Set(CreateFileW(log.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,
            restart ? OPEN_ALWAYS : CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
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
        // 個体は DriverHost が列挙したデバイスインターフェースのパスで指定する
        std::wstring command=Quote(helper_)+L" serve --path "+Quote(device_path_)+L" --frequency "+std::to_wstring(frequency_)
            +L" --gain "+std::to_wstring(system_j83b_ ? gain_j83b_ : gain_);
        if(system_j83b_)command+=L" --system j83b";
        else if(viterbi_threads_)command+=L" --viterbi-threads "+std::to_wstring(viterbi_threads_);
        PROCESS_INFORMATION process{};
        const BOOL created=attr_ok && CreateProcessW(helper_.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if(!created){msg_err("MSi2500: CreateProcess failed win32=%lu\n",GetLastError());return fail(-EIO);}
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
        // helper は停止要求の後に USB の転送を止め、停止要求と ADC の休止を送ってから終わる。
        // この順序を省くと次回の取り込みでデータが届かない状態に入ることがあったため、強制終了までの期限は長めに取る。
        // drain_only_ を立てた後は監視のスレッドがハンドルを入れ替えないため、ロックの外で待てる
        HANDLE process=nullptr,job=nullptr;
        {
            std::lock_guard<std::mutex> handles(handle_mutex_);
            drain_only_=true;input_.Set(nullptr);
            process=process_.Get();job=job_.Get();
        }
        if(process && WaitForSingleObject(process,5000)==WAIT_TIMEOUT){
            msg_warn("MSi2500: helper stop deadline expired; terminating owned job\n");
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
     * 機器のデバイスインターフェースがまだあるか。抜去では helper が転送の失敗で先に終わり、
     * DriverHost が抜去の通知を受け取るのはその後になる (実機では helper の失敗が先だった)。
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
     * helper が予期せず終わったとき (USB の受信が止まる、転送の失敗が続く、異常終了、応答なし、形式不正)、
     * 同じ周波数で helper を起動し直す。配信の接続と世代は保ち、欠けた区間は配信先が continuity counter で
     * 検出する。打ち切ると選局し直すまで録画が止まるため、PX-BCUD の自動復旧と同じ回数の制限と待機で続ける。
     * 回数を使い切った後も配信の接続は残し、CheckLock の失敗で知らせて選局し直しを待つ。
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
            if(exit_code==HelperArgumentError){
                Exhausted("helper rejected its arguments",exit_code);
                break;
            }
            bool started=false;
            while(!started) {
                if(!budget.Take()){
                    Exhausted("auto restart exhausted",exit_code);
                    return;
                }
                const auto backoff=budget.Backoff();
                for(auto waited=std::chrono::milliseconds::zero();waited<backoff && !Stopping();waited+=std::chrono::milliseconds(10))Sleep(10);
                // パスが消えていれば抜去の通知を最長 3 秒待つ。期限を過ぎてもないときは起動し直しの失敗として数える
                if(!DevicePresent()){
                    for(int i=0;i<300 && !Stopping() && !DevicePresent();++i)Sleep(10);
                    if(Stopping())return;
                    if(!DevicePresent()){msg_warn("MSi2500: device interface is missing; not restarting the helper\n");continue;}
                }
                std::lock_guard<std::mutex> handles(handle_mutex_);
                if(Stopping())return;
                ++restart_count_;
                msg_warn("MSi2500: helper restart count=%llu exit_code=%lu consecutive=%u backoff_ms=%lld possible_ts_loss=1\n",
                    static_cast<unsigned long long>(restart_count_),exit_code,static_cast<unsigned>(budget.Consecutive()),
                    static_cast<long long>(backoff.count()));
                started=!StartProcess(true);
                if(!started)msg_err("MSi2500: helper restart failed count=%llu\n",static_cast<unsigned long long>(restart_count_));
            }
            restarted_at_=failed_at;
        }
    }
    void Exhausted(const char *reason,DWORD exit_code)
    {
        worker_error_=true;quality_=0;
        msg_err("MSi2500: %s exit_code=%lu restart_count=%llu limit=%u; waiting for a retune\n",reason,exit_code,
            static_cast<unsigned long long>(restart_count_),static_cast<unsigned>(px4::pxbcud::RecoveryBudget::Limit));
    }
    struct HelperRun {std::uint64_t received_ms=0;};
    // 1 つの helper の出力を、終わるか停止を求められるまで読む。最初と最後の TS の間を返す
    HelperRun ReadHelper() noexcept
    {
        HelperRun run;
        try {
            Frames frames;std::array<std::uint8_t,65536> bytes{};
            px4::StreamOverflowLog overflow("MSi2500");
            auto last_message=GetTickCount64();
            ULONGLONG first_ts=0;
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
                        if(frame.kind==3){
                            quality_=frame.quality;
                            // MER は TS 出力中だけ載る。載らない状態フレームでは直前の値を残し、ロックの判定は TS の到着で行う。
                            std::uint32_t mer=0;
                            if(px4::rtlsdr::StatusMer(frame,data,mer))mer_x100_=static_cast<std::int32_t>(mer);
                        }
                        if(frame.kind==2){
                            quality_=frame.quality;last_ts_=GetTickCount64();
                            if(!first_ts)first_ts=last_ts_;
                            run.received_ms=last_ts_-first_ts;
                            if(const auto restarted=restarted_at_.exchange(0)){
                                msg_warn("MSi2500: helper restart resumed count=%llu outage_ms=%llu possible_ts_loss=1\n",
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
                    offset+=part;
                }
                if(!valid){msg_err("MSi2500: invalid or failed helper frame\n");break;}
            }
        } catch(const std::exception &error){msg_err("MSi2500: reader failed: %s\n",error.what());}
        if(!Stopping())msg_err("MSi2500: frontend stopped unexpectedly\n");
        return run;
    }
    std::wstring device_path_;
    std::uintptr_t index_;
    std::wstring helper_;
    bool async_tune_=false;
    bool stream_started_=false; // mutex_ で保護し、Close まで配信バッファを維持する
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
    // 復調器の MER (dB×100)。helper の起動と停止で破棄し、未受信の間は -1
    std::atomic<std::int32_t> mer_x100_{-1};
    std::atomic<ULONGLONG> last_ts_{0};
    // helper のハンドル。監視のスレッドが起動し直すときに入れ替えるため、handle_mutex_ で守る。
    // 取得順は mutex_、handle_mutex_ の順で、監視のスレッドは handle_mutex_ だけを取る
    std::mutex handle_mutex_;
    Handle process_,job_,input_,output_;
    // 起動し直した回数 (handle_mutex_ を保持して更新する) と、起動し直した後に最初の TS を待つ間の失敗の時刻。
    // 回数は ReadHelper が handle_mutex_ を取らずにログへ出すため、atomic にする
    std::atomic<std::uint64_t> restart_count_{0};
    std::atomic<ULONGLONG> restarted_at_{0};
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
