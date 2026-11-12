// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "frontend.hpp"
#include "capture_gate.hpp"
#include "firmware_path.hpp"
#include "iq_helper.hpp"
#include "ts_diagnostic_log.hpp"
#include "msg.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>

namespace px4::siano {
namespace {
using Clock=std::chrono::steady_clock;
// 上流 Windows と同じ 32 x 16 KiB。制御応答と TS は同じ bulk IN を通る。
constexpr std::size_t TransferSize=16384, TransferCount=32;
// 受信の開始時に読み飛ばす、前のホストの残りの転送の上限。実機の残りは 1 回の転送だった。
// 上限を超えても解釈できない場合は、残りではなく形式の異常として受信を止める
constexpr unsigned MaxStaleTransfers=4;
int UsbError(DWORD e)
{
    if (e==ERROR_DEVICE_NOT_CONNECTED || e==ERROR_OPERATION_ABORTED || e==ERROR_INVALID_HANDLE) return -ECANCELED;
    if (e==ERROR_TIMEOUT || e==ERROR_SEM_TIMEOUT) return -ETIMEDOUT;
    return -EIO;
}
std::filesystem::path HostDirectory()
{
    std::array<wchar_t,32768> path{};
    auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!n || n==path.size()) throw DeviceError("PX-S1UD: cannot resolve DriverHost directory");
    return std::filesystem::path(path.data()).parent_path();
}
int ReadFirmware(const std::filesystem::path& path,std::vector<std::uint8_t>& firmware)
{
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if (!f) return -ENOENT;
    auto n=f.tellg();
    if (n<=0 || n>4*1024*1024) return -EBADMSG;
    firmware.resize(static_cast<std::size_t>(n)); f.seekg(0);
    return f.read(reinterpret_cast<char*>(firmware.data()),n) ? 0 : -EIO;
}
}
class Receiver final : public ReceiverBase, private Io {
public:
    // J.83B は、利用者が用意した IQ を送出する firmware と helper を指定したときだけ受け付ける
    struct J83bConfig {std::filesystem::path firmware,helper,log;};
    Receiver(HANDLE file,WINUSB_INTERFACE_HANDLE usb,HANDLE removed,UCHAR in,std::size_t alignment,
             const std::filesystem::path& directory,const std::filesystem::path& firmware,const J83bConfig* j83b)
        : ReceiverBase(0),file_(file),usb_(usb),removed_(removed),in_(in),alignment_(alignment),
          host_directory_(directory),firmware_(firmware),frontend_(*this)
    {
        if (j83b) {j83b_=*j83b;j83b_supported_=true;}
    }
    ~Receiver() override
    {
        // ReceiverManager が貸し出した生ポインターの Close 完了まで寿命を保持する。
        std::unique_lock<std::mutex> lock(mutex_);
        closed_.wait(lock,[this]{return !open_;});
        StopWorker();
    }
    void Disconnected()
    {
        stream_buf_->StopRequest();
        responses_changed_.notify_all();
    }
    int Open() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) return -EBUSY;
        if (Removed()) return -ECANCELED;
        // 配置変更後の再オープンでも、INI 指定先・EXE 直下・配布用サブフォルダーを再探索する。
        const auto firmware_path=ResolveFirmwarePath(host_directory_,firmware_);
        std::vector<std::uint8_t> firmware;
        const int read=ReadFirmware(firmware_path,firmware);
        if (read && read!=-ENOENT) return read;
        if (read) {
            for (const auto& candidate:FirmwareCandidates(host_directory_,firmware_))
                msg_warn("PX-S1UD: firmware candidate: %ls\n",candidate.c_str());
            msg_warn("PX-S1UD: firmware not found; only an already initialized device can open\n");
        }
        int ret=StartWorker();
        if (!ret) ret=frontend_.Initialize(firmware);
        if (ret==-EIO && !firmware.empty()) {
            // J.83B 用の firmware が稼働したままの機器 (この DriverHost や BonDriver_S1UD.dll が読み込んだもの) は、
            // ISDB-T の INIT_DEVICE を受けても ISDB-T のモードへ移らない。通常の firmware を読み込み直す
            msg_warn("PX-S1UD: device did not enter ISDB-T mode; reloading the ISDB-T firmware\n");
            ret=frontend_.Initialize(firmware,true);
        }
        if (ret) {
            msg_err("PX-S1UD: initialization failed ret=%d firmware=%ls\n",ret,firmware_path.c_str());
            StopWorker(); return ret;
        }
        open_=true; tuned_=false; capture_=false; locked_=false; cnr_=0;
        signal_log_=(msg_get_mode() & (MSG_MODE_LOG_FILE | MSG_MODE_CONSOLE))!=0; signal_={}; signal_base_={};
        msg_info("PX-S1UD: opened firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
        return 0;
    }
    void Close() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        capture_=false; tuned_=false; DisableOutput();
        // IQ の送出を止めてから受信を止める。送出を止めずに閉じると、次の open で IQ のメッセージが
        // 制御応答と同じ bulk IN を埋め続ける
        StopIq();
        // J.83B 用の firmware のまま閉じると、受信を止めて開き直した後の GET_VERSION に応答しなかった。
        // 他のホストやアプリも ISDB-T の firmware を前提に開くため、受信を動かしている間に通常の firmware へ戻す
        if (loaded_==Loaded::J83b && !error_ && !Removed() && ReloadIsdbt())
            msg_warn("PX-S1UD: closed with the J.83B firmware still running\n");
        stream_buf_->Stop(); StopWorker(); open_=false; closed_.notify_all();
    }
    int SetLnbVoltage(std::int32_t voltage) override { return voltage ? -ENOSYS : 0; }
    int SetCapture(bool capture) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (capture==capture_) return capture ? -EALREADY : 0;
        if (capture) {
            if (!stream_buf_->Alloc(188*8192)) return -ENOMEM;
            stream_buf_->SetThresholdSize(188*32);
            stream_buf_->SetDiscardNullPacketsOnOutput(false);
            stream_buf_->Start(); capture_=true;
            if (iq_) iq_output_=true;
            else if (tuned_ && locked_) EnableOutput();
        } else {
            capture_=false; DisableOutput(); iq_output_=false; stream_buf_->Stop();
        }
        return error_.load();
    }
    int CheckLock(bool& locked) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked=false;
        if (!open_) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (error_) return error_;
        if (!tuned_) return 0;
        if (iq_) {
            // J.83B の復調は helper が行い、firmware の統計は ISDB-T のもののため使わない。
            // MSi2500 と同じく、helper の TS が 3 秒以内に届いていればロックとする。
            // C/N には helper が報告する復調器の MER (dB) を ISDB-T の CNR と同じ dB×1000 で返す。
            // RS の成功割合は正常受信中にほぼ 100% で変わらず、C/N の目安にならないため。
            // MER をまだ報告していない間 (起動直後と MER を送らない古い helper) は、従来どおり RS の成功割合を返す
            if (iq_helper_.Failed()) return -ECANCELED;
            const auto last=iq_helper_.LastTs();
            locked_=iq_helper_.Ready() && last && GetTickCount64()-last<3000;
            const auto mer=iq_helper_.MerX100();
            cnr_=!locked_ ? 0 : mer>=0 ? mer*10 : static_cast<std::int32_t>(iq_helper_.Quality()*10);
            locked=locked_;
            return 0;
        }
        const auto now=Clock::now();
        if (now-last_stats_>=std::chrono::milliseconds(100)) {
            std::vector<std::uint8_t> p;
            int ret=frontend_.RequestStats(p);
            if (!ret) ret=frontend_.DecodeStats(p,locked_,cnr_);
            if (ret) {
                error_=ret;stream_buf_->StopRequest();
                msg_err("PX-S1UD: statistics failed ret=%d\n",ret);
                return ret;
            }
            if (signal_log_) LogSignal(p);
            last_stats_=Clock::now();
        }
        locked=locked_;
        if (locked && capture_ && !output_) EnableOutput();
        return 0;
    }
    int ReadStat(command::StatType type,std::int32_t& value) override
    {
        bool locked=false;
        int ret=CheckLock(locked);
        if (ret) return ret;
        std::lock_guard<std::mutex> lock(mutex_);
        value=0;
        if (type!=command::StatType::CNR) return -ENOSYS;
        value=locked ? cnr_ : 0; return 0;
    }
protected:
    int SetFrequency() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool j83b=params_.system==SystemType::J83B;
        if (!open_) return -EINVAL;
        if (j83b) {
            // J.83B はチャンネル定義の中心周波数 (kHz) をそのまま使う (MSi2500 と同じ範囲)
            if (!j83b_supported_ || params_.freq<90000 || params_.freq>775000) return -EINVAL;
        } else if (params_.system!=SystemType::ISDB_T || (params_.bandwidth && params_.bandwidth!=6)) return -EINVAL;
        if (Removed()) return -ECANCELED;
        if (error_) return error_;
        DisableOutput(); tuned_=false; locked_=false; cnr_=0;
        // 前のチャンネルの IQ の送出と helper を止めてから世代を切り替え、前の TS と復調状態を次へ持ち込まない
        int ret=StopIq();
        // Siano の制御応答と TS は同じ bulk IN を通る。選局ごとの取消・flush は
        // メッセージ途中の USB パケットを残し、再開後に TS の端数をヘッダーとして
        // 読むことがある。受信要求を継続し、Gate の世代で選局前とロック前の TS を除く。
        if (!ret && !stream_buf_->AdvanceEpoch()) return -EIO;
        if (!ret) ret=j83b ? TuneJ83b() : TuneIsdbt();
        if (ret) {
            msg_err("PX-S1UD: tune failed system=%s frequency_khz=%u ret=%d\n",j83b ? "J.83B" : "ISDB-T",params_.freq,ret);
            // 失敗した後の要求と応答の対応は保証できないため、この open を失敗状態にする
            StopIq();
            error_=ret;stream_buf_->StopRequest();
            return ret;
        }
        tuned_=true; last_stats_=Clock::time_point{}; signal_={}; signal_base_={};
        return 0;
    }
    int SetStreamId() override { return 0; }
private:
    bool Removed() const { return WaitForSingleObject(removed_,0)==WAIT_OBJECT_0; }
    // J.83B 用の firmware から通常の firmware へ読み込み直す
    int ReloadIsdbt()
    {
        std::vector<std::uint8_t> firmware;
        const auto path=ResolveFirmwarePath(host_directory_,firmware_);
        int ret=ReadFirmware(path,firmware);
        if (!ret) ret=frontend_.Initialize(firmware,true);
        if (ret) {msg_err("PX-S1UD: ISDB-T firmware reload failed ret=%d firmware=%ls\n",ret,path.c_str());return ret;}
        loaded_=Loaded::Isdbt;
        msg_info("PX-S1UD: ISDB-T firmware reloaded firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
        return 0;
    }
    int TuneIsdbt()
    {
        if (loaded_==Loaded::J83b) {
            const int ret=ReloadIsdbt();
            if (ret) return ret;
        }
        return frontend_.Tune(params_.freq);
    }
    int TuneJ83b()
    {
        if (loaded_!=Loaded::J83b) {
            // J.83B 用の firmware は配布物に含めず、利用者が J83BFirmwarePath で指定する
            std::vector<std::uint8_t> firmware;
            int ret=ReadFirmware(j83b_.firmware,firmware);
            if (ret) {msg_err("PX-S1UD: J.83B firmware read failed ret=%d firmware=%ls\n",ret,j83b_.firmware.c_str());return ret;}
            ret=frontend_.Initialize(firmware,true,false);
            if (ret) {msg_err("PX-S1UD: J.83B firmware load failed ret=%d\n",ret);return ret;}
            loaded_=Loaded::J83b;
            msg_info("PX-S1UD: J.83B firmware loaded firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
        }
        // helper を先に起動し、IQ の送出の始めのブロックから渡す
        int ret=iq_helper_.Start(j83b_.helper.wstring(),j83b_.log.wstring(),
            [this](const std::uint8_t* data,std::size_t size) {
                if (!iq_output_) return true;
                std::size_t bytes=size;
                if (!stream_buf_->Write(data,bytes) || bytes!=size) {
                    msg_err("PX-S1UD: TS buffer overflow bytes=%zu accepted=%zu\n",size,bytes);
                    return false;
                }
                stream_buf_->NotifyWrite();
                return true;
            },
            // 読み取りのスレッドから呼ばれるため、mutex_ を取らずに配信の停止だけを要求する
            [this] {stream_buf_->StopRequest();});
        if (ret) {msg_err("PX-S1UD: J.83B helper start failed ret=%d helper=%ls\n",ret,j83b_.helper.c_str());return ret;}
        iq_min_pending_=static_cast<unsigned>(TransferCount); iq_max_resubmit_us_=0;
        iq_feed_=true;
        ret=frontend_.SetIqStream(params_.freq*1000);
        if (ret) {
            if (ret==-ETIMEDOUT) msg_err("PX-S1UD: the firmware does not answer the IQ stream request\n");
            return ret;
        }
        iq_=true; iq_output_=capture_;
        msg_info("PX-S1UD: J.83B started frequency_khz=%u\n",params_.freq);
        return 0;
    }
    // IQ の送出と helper を止める。送出の停止要求の失敗は、この open の失敗として返す
    int StopIq()
    {
        int ret=0;
        iq_output_=false;
        if (iq_) {
            iq_=false;
            std::vector<std::uint8_t> p;
            if (!error_ && !Removed()) ret=frontend_.SetIqStream(0,&p);
            // 停止の応答は送出中のチップのバッファーの統計。BonDriver_S1UD.dll のログの書式に合わせると、
            // 結果、溜めた数、送った数、不明、捨てた数、タイムアウト、エラーの順 (実機で filled - sent = dropped)。
            // チップが捨てたブロックは USB へ送り切れなかったもので、ホストの受信要求が尽きたか (min_pending が 0) と
            // あわせて欠落の原因の切り分けに使う
            const auto w=[&](std::size_t i) {return p.size()>=4*(i+1) ? sms_get_le32(p.data()+4*i) : 0U;};
            msg_info("PX-S1UD: J.83B chip result=%u filled=%u sent=%u dropped=%u timeouts=%u errors=%u bytes=%zu "
                "host min_pending=%u max_resubmit_us=%llu\n",w(0),w(1),w(2),w(4),w(5),w(6),p.size(),
                iq_min_pending_.load(),static_cast<unsigned long long>(iq_max_resubmit_us_.load()));
        }
        iq_feed_=false;
        if (iq_helper_.Running()) {
            const auto dropped=iq_helper_.Dropped();
            iq_helper_.Stop();
            if (dropped) msg_warn("PX-S1UD: J.83B helper queue dropped %llu blocks\n",static_cast<unsigned long long>(dropped));
        }
        return ret;
    }
    // 受信品質を TS 異常と同じ時刻軸で照合する。統計はアプリの信号レベル取得で送る既存要求を流用し、
    // TS と同じ bulk IN を使う制御転送を増やさない。誤り・ロック外れの応答は即時に、
    // それ以外は約10秒ごとに集計する。解釈できない応答でも受信は止めない。
    // 実機の err・total・tmcc はロック時に0へ戻る累積値のため、前回応答からの増分で判定・集計する。
    void LogSignal(const std::vector<std::uint8_t>& p)
    {
        SignalStats s{};
        if (const int ret=frontend_.DecodeSignal(p,s)) {
            if (!signal_.decode_warned) msg_warn("PX-S1UD: signal stats unavailable bytes=%zu ret=%d\n",p.size(),ret);
            signal_.decode_warned=true;
            return;
        }
        const auto now=Clock::now();
        if (!signal_.polls) {signal_.window=now;signal_.min_snr=signal_.max_snr=s.snr;}
        ++signal_.polls; if (!s.demod_locked) ++signal_.unlocked;
        signal_.min_snr=(std::min)(signal_.min_snr,s.snr); signal_.max_snr=(std::max)(signal_.max_snr,s.snr);
        signal_.frequency=s.frequency; signal_.layers=s.layers;
        // 初回は基準だけを取り、値が減った場合はロックし直しによる0からの再計数とみなす。
        const auto delta=[this](std::uint32_t current,std::uint32_t& previous) {
            const auto d=!signal_base_.valid ? 0U : current>=previous ? current-previous : current;
            previous=current; return d;
        };
        bool bad=!s.demod_locked;
        char detail[384]={}; std::size_t used=0;
        for (std::size_t i=0;i<3;++i) {
            const auto& l=s.layer[i];
            if (l.segments>13) continue;
            const auto err=delta(l.error_ts,signal_base_.error_ts[i]);
            const auto total=delta(l.total_ts,signal_base_.total_ts[i]);
            const auto tmcc=delta(l.tmcc_errors,signal_base_.tmcc[i]);
            signal_.error_ts[i]+=err; signal_.total_ts[i]+=total; signal_.tmcc[i]+=tmcc;
            bad=bad || err || tmcc;
            const int n=sprintf_s(detail+used,sizeof(detail)-used," %c:seg=%u err=+%u(%u) total=+%u(%u) ber=%lld tmcc=+%u(%u)",
                static_cast<char>('A'+i),l.segments,err,l.error_ts,total,l.total_ts,
                l.ber==0xffffffff ? -1LL : static_cast<long long>(l.ber),tmcc,l.tmcc_errors);
            if (n>0) used+=static_cast<std::size_t>(n);
        }
        signal_base_.valid=true;
        if (bad) msg_warn("PX-S1UD: signal errors rf_locked=%d demod_locked=%d snr=%d rssi=%d%s\n",
            s.rf_locked,s.demod_locked,s.snr,s.rssi,detail);
        if (now-signal_.window<std::chrono::seconds(10)) return;
        char summary[256]={}; used=0;
        for (std::size_t i=0;i<3;++i) {
            if (!signal_.total_ts[i] && !signal_.error_ts[i] && !signal_.tmcc[i]) continue;
            const int n=sprintf_s(summary+used,sizeof(summary)-used," %c:err=%llu total=%llu tmcc=%llu",
                static_cast<char>('A'+i),signal_.error_ts[i],signal_.total_ts[i],signal_.tmcc[i]);
            if (n>0) used+=static_cast<std::size_t>(n);
        }
        msg_info("PX-S1UD: signal summary polls=%u unlocked=%u snr_min=%d snr_max=%d frequency=%u layers=%u%s\n",
            signal_.polls,signal_.unlocked,signal_.min_snr,signal_.max_snr,signal_.frequency,signal_.layers,summary);
        const bool warned=signal_.decode_warned; signal_={}; signal_.decode_warned=warned;
    }
    void DisableOutput()
    {
        // Feed/Write と同じロックで閉じ、呼び出しから戻った後の旧 TS 書込みを防ぐ。
        gate_.SetEnabled(false); output_=false;
    }
    void EnableOutput()
    {
        gate_.SetEnabled(true); output_=true;
    }
    void Reloading(bool on) override
    {
        if (on) reload_recoveries_=0;
        reloading_=on;
    }
    int Delay(unsigned ms) override
    {
        return WaitForSingleObject(removed_,ms)==WAIT_TIMEOUT ? 0 : -ECANCELED;
    }
    int Exchange(const std::vector<std::uint8_t>& message,std::uint16_t response,
                 std::vector<std::uint8_t>& payload,unsigned ms) override
    {
        if (Removed()) return -ECANCELED;
        if (error_) return error_;
        const auto deadline=Clock::now()+std::chrono::milliseconds(ms);
        {
            std::lock_guard<std::mutex> lock(response_mutex_);
            expected_=response; received_=false; response_.clear();
        }
        OVERLAPPED op{}; op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!op.hEvent) return -ENOMEM;
        ULONG bytes=0;
        BOOL ok=WinUsb_WritePipe(usb_,2,const_cast<PUCHAR>(message.data()),static_cast<ULONG>(message.size()),nullptr,&op);
        DWORD e=ok ? ERROR_SUCCESS : GetLastError();
        if (ok || e==ERROR_IO_PENDING) {
            HANDLE events[]={removed_,op.hEvent};
            auto result=ok ? WAIT_OBJECT_0+1 : WaitForMultipleObjects(2,events,FALSE,ms);
            if (result!=WAIT_OBJECT_0+1) {
                CancelIoEx(file_,&op); WinUsb_GetOverlappedResult(usb_,&op,&bytes,TRUE);
                ok=FALSE; e=result==WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
            } else {
                ok=WinUsb_GetOverlappedResult(usb_,&op,&bytes,FALSE);
                e=ok ? ERROR_SUCCESS : GetLastError();
            }
        }
        CloseHandle(op.hEvent);
        int ret=ok ? (bytes==message.size() ? 0 : -EIO) : UsbError(e);
        if (!ret && response) {
            std::unique_lock<std::mutex> lock(response_mutex_);
            if (!responses_changed_.wait_until(lock,deadline,[&]{return received_ || error_ || Removed();})) ret=-ETIMEDOUT;
            else if (Removed()) ret=-ECANCELED;
            else if (error_) ret=error_;
            else payload=response_;
            expected_=0;
        }
        if (ret) {
            // 通し番号のない応答を次の要求へ誤対応させないため、この open を失敗状態にする。
            error_=ret; stream_buf_->StopRequest();
            msg_err("PX-S1UD: command=%u response=%u failed ret=%d win32=%lu\n",sms_get_le16(message.data()),response,ret,e);
        }
        return ret;
    }
    int StartWorker()
    {
        if (Removed()) return -ECANCELED;
        if (worker_.joinable()) return -EALREADY;
        error_=0; stop_=false;
        if (!WinUsb_ResetPipe(usb_,in_) || !WinUsb_ResetPipe(usb_,2) || !WinUsb_FlushPipe(usb_,in_)) {
            msg_err("PX-S1UD: pipe reset/flush failed in=0x%02x win32=%lu\n",in_,GetLastError());
            return -EIO;
        }
        try { worker_=std::thread([this]{Run();}); }
        catch (...) {stop_=true;return -ENOMEM;}
        return 0;
    }
    void StopWorker()
    {
        stop_=true;
        if (worker_.joinable()) { WinUsb_AbortPipe(usb_,in_); worker_.join(); }
    }
    void Run() noexcept
    {
        // J.83B では同じ PC で IQ の復調が CPU を使い続け、通常の優先度では受信要求の出し直しが最大 64 ms 遅れた。
        // チップの送信枠は約 7 ms 分しかないため、TS の配信スレッドと同じく優先度を上げて読み取りを途切れさせない
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
        try { RunTransfers(); }
        catch (const std::bad_alloc&) { error_=-ENOMEM;msg_err("PX-S1UD: receive allocation failed\n"); }
        catch (const std::exception& e) { error_=-EIO;msg_err("PX-S1UD: receive exception: %s\n",e.what()); }
        catch (...) { error_=-EIO;msg_err("PX-S1UD: unknown receive exception\n"); }
        if (error_) {stream_buf_->StopRequest();responses_changed_.notify_all();}
    }
    void RunTransfers()
    {
        struct Read {
            OVERLAPPED op{};
            std::array<std::uint8_t,TransferSize> data{};
            bool pending=false;
            std::uint64_t generation=0;
        };
        // 32要求は x86 の既定スタックを圧迫するためヒープへ確保する。
        auto reads=std::make_unique<std::array<Read,TransferCount>>();
        // 例外経路でも取消完了まで OVERLAPPED とバッファの寿命を保つ。
        struct Cleanup {
            Receiver& owner; std::array<Read,TransferCount>& reads;
            ~Cleanup() {
                for (auto& r:reads) {
                    if (r.pending) {CancelIoEx(owner.file_,&r.op);ULONG n=0;WinUsb_GetOverlappedResult(owner.usb_,&r.op,&n,TRUE);}
                    if (r.op.hEvent) CloseHandle(r.op.hEvent);
                }
            }
        } cleanup{*this,*reads};
        TsDiagnosticLog diagnostics("siano_usb_input");
        const bool diagnostic_enabled=(msg_get_mode() & (MSG_MODE_LOG_FILE | MSG_MODE_CONSOLE))!=0;
        diagnostics.Enable(diagnostic_enabled);
        diagnostics.SetReadyProbe([](void* context) noexcept {
            const auto& requests=*static_cast<std::array<Read,TransferCount>*>(context);
            int ready=0;
            for (const auto& request:requests)
                if (request.pending && WaitForSingleObject(request.op.hEvent,0)==WAIT_OBJECT_0) ++ready;
            return ready;
        },reads.get());
        TsTransferTracer tracer;
        auto last_submit=Clock::now();
        std::uint64_t last_gap_us=0;
        auto submit=[&](Read& r) {
            if (stop_ || Removed()) return false;
            ResetEvent(r.op.hEvent); r.generation=gate_.Ticket();
            BOOL ok=WinUsb_ReadPipe(usb_,in_,r.data.data(),static_cast<ULONG>(r.data.size()),nullptr,&r.op);
            if (!ok && GetLastError()!=ERROR_IO_PENDING) {
                const auto e=GetLastError();error_=UsbError(e);
                msg_err("PX-S1UD: USB read submission failed endpoint=0x%02x win32=%lu\n",in_,e);
                return false;
            }
            if (diagnostic_enabled) {
                const auto now=Clock::now();
                last_gap_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-last_submit).count());
                last_submit=now;
            }
            r.pending=true;return true;
        };
        for (auto& r:*reads) {
            r.op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!r.op.hEvent) {error_=-ENOMEM;return;}
            if (!submit(r)) return;
        }
        std::uint64_t malformed=0,iq_malformed=0,iq_blocks_seen=0;
        // 受信の開始後に最初のメッセージを解釈したか、それまでに読み飛ばした転送の数
        bool synchronized=false; unsigned stale=0;
        Clock::time_point last_iq{};
        std::size_t next=0;
        while (!stop_ && !Removed() && !error_) {
            auto& r=(*reads)[next];
            HANDLE events[]={removed_,r.op.hEvent};
            auto result=WaitForMultipleObjects(2,events,FALSE,100);
            if (result==WAIT_TIMEOUT) continue;
            if (result!=WAIT_OBJECT_0+1) {
                if (!stop_) {
                    error_=result==WAIT_OBJECT_0 ? -ECANCELED : -EIO;
                    msg_err("PX-S1UD: USB receive wait failed result=%lu win32=%lu\n",result,GetLastError());
                }
                break;
            }
            ULONG n=0;
            if (!WinUsb_GetOverlappedResult(usb_,&r.op,&n,FALSE)) {
                const auto e=GetLastError();
                // firmware の読み込み直しで J.83B 用の firmware が ROM へ戻るとき、受信が ERROR_GEN_FAILURE で失敗し、
                // パイプのリセットで残りの要求も取り消される。BonDriver_S1UD.dll と同じく IN をリセットして要求を出し直す。
                // 読み込み直しの間だけ、要求の数の 4 倍までに限る
                if (!stop_ && reloading_ && (e==ERROR_GEN_FAILURE || e==ERROR_OPERATION_ABORTED) &&
                    reload_recoveries_<TransferCount*4) {
                    ++reload_recoveries_;r.pending=false;
                    if (e==ERROR_GEN_FAILURE) {
                        msg_warn("PX-S1UD: USB read failed during firmware reload win32=%lu; resetting the IN pipe\n",e);
                        WinUsb_ResetPipe(usb_,in_);
                    }
                    if (!submit(r)) break;
                    next=(next+1)%TransferCount;
                    continue;
                }
                SetLastError(e);
                if (!stop_) {
                    const auto e=GetLastError();error_=UsbError(e);
                    msg_err("PX-S1UD: USB read completion failed endpoint=0x%02x win32=%lu\n",in_,e);
                }
                break;
            }
            r.pending=false;
            // 異常の前後で転送単位の欠落・入替えを判別するため、完了順にメタデータを残す。
            TsTransferTrace* trace=nullptr;
            const auto anomalies=diagnostics.AnomalyCount();
            if (diagnostic_enabled) {
                TsTransferTrace t{};FILETIME ft{};GetSystemTimePreciseAsFileTime(&ft);
                t.completed_at=(static_cast<std::uint64_t>(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
                t.slot=static_cast<unsigned>(next);t.bytes=n;trace=&tracer.Begin(t);
            }
            sms_frame frame{};
            const auto parse=n ? sms_frame_message(r.data.data(),n,alignment_,&frame) : 0;
            if (parse && !synchronized && stale<MaxStaleTransfers) {
                // 前のホストが受信を止めたとき、メッセージの途中で取り消した読み取りの残りは機器側に残り、
                // ホストのパイプのリセットでは消えない。実機では、先頭が TS の途中の 1 回の転送だけで、
                // その後は境界から読めた。最初のメッセージを解釈するまでは、この残りを読み飛ばす
                ++stale;
                msg_warn("PX-S1UD: skipped a stale USB transfer bytes=%lu ret=%d\n",n,parse);
            } else if (n) {
                if (parse) {
                    ++malformed;error_=-EBADMSG;
                    msg_err("PX-S1UD: invalid USB frame bytes=%lu alignment=%zu ret=%d\n",n,alignment_,parse);
                    break;
                }
                synchronized=true;
                if (trace) {trace->frame_type=frame.type;trace->frame_length=frame.length;trace->frame_flags=frame.flags;}
                const auto* p=r.data.data()+frame.payload_offset;
                if (frame.type==MSG_IQ_DATA) {
                    // J.83B の IQ のブロック。復調は helper が行い、ここでは待たずにキューへ積む
                    if (frame.payload_length!=IqBlockBytes) ++iq_malformed;
                    else if (iq_feed_) {
                        iq_helper_.Feed(p,frame.payload_length);
                        // 欠落の切り分け用: 前の IQ の処理からの間隔の最大と、8 ブロックごとに数えた、
                        // ホストコントローラーに残っている (未完了の) 受信要求の最小数
                        const auto now=Clock::now();
                        if (iq_blocks_seen++) {
                            const auto us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-last_iq).count());
                            if (us>iq_max_resubmit_us_) iq_max_resubmit_us_=us;
                        }
                        last_iq=now;
                        if (!(iq_blocks_seen&7)) {
                            unsigned outstanding=0;
                            for (const auto& q:*reads) if (q.pending && WaitForSingleObject(q.op.hEvent,0)!=WAIT_OBJECT_0) ++outstanding;
                            if (outstanding<iq_min_pending_) iq_min_pending_=outstanding;
                        }
                    }
                } else if (frame.type==MSG_SMS_DVBT_BDA_DATA) {
                    if (!gate_.Feed(r.generation,p,frame.payload_length,[this,&diagnostics,&r,&last_submit,&last_gap_us,diagnostic_enabled,trace](const std::uint8_t* data,std::size_t size) {
                        // 異常検出とメタデータだけを記録し、TS 本体と転送条件には触れない。
                        if (diagnostic_enabled) {
                            const auto elapsed=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-last_submit).count());
                            diagnostics.Context((std::max)(elapsed,last_gap_us));
                            // framer の再同期で捨てたバイトは TS に残らないため、CC 異常と並べて記録する。
                            diagnostics.Source(gate_.DiagnosticsInWrite().discarded);
                            diagnostics.Feed(data,size,r.generation);
                            if (trace && size>=188) {
                                const auto* last=data+(size/188-1)*188;
                                trace->packets=static_cast<unsigned>(size/188);
                                trace->first_pid=((data[1]&31)<<8)|data[2];trace->first_cc=data[3]&15;
                                trace->last_pid=((last[1]&31)<<8)|last[2];trace->last_cc=last[3]&15;
                            }
                        }
                        std::size_t bytes=size;
                        const bool written=stream_buf_->Write(data,bytes);
                        stream_buf_->NotifyWrite();
                        // Purge 中は true と短い長さで意図した破棄を返す。満杯の false と区別する。
                        if (!written) {
                            msg_err("PX-S1UD: TS buffer overflow bytes=%zu accepted=%zu\n",size,bytes);
                            return false;
                        }
                        return true;
                    })) error_=-ENOBUFS;
                } else {
                    std::lock_guard<std::mutex> lock(response_mutex_);
                    if (frame.type==expected_) {
                        response_.assign(p,p+frame.payload_length);received_=true;responses_changed_.notify_all();
                    }
                }
            }
            if (trace) tracer.End(diagnostics,diagnostics.AnomalyCount()!=anomalies,GetTickCount64());
            if (!submit(r)) break;
            next=(next+1)%TransferCount;
        }
        diagnostics.Reset();
        const auto& framer=gate_.Diagnostics();
        msg_info("PX-S1UD: RX stopped packets=%llu tei=%llu discarded=%llu malformed=%llu iq_malformed=%llu ret=%d\n",
            static_cast<unsigned long long>(framer.packets),static_cast<unsigned long long>(framer.tei),
            static_cast<unsigned long long>(framer.discarded),static_cast<unsigned long long>(malformed),
            static_cast<unsigned long long>(iq_malformed),error_.load());
    }
    HANDLE file_; WINUSB_INTERFACE_HANDLE usb_; HANDLE removed_; UCHAR in_; std::size_t alignment_;
    std::filesystem::path host_directory_,firmware_; Frontend frontend_;
    std::mutex mutex_,response_mutex_; std::condition_variable closed_,responses_changed_;
    bool open_=false,capture_=false,tuned_=false,locked_=false;
    std::int32_t cnr_=0; Clock::time_point last_stats_{};
    struct SignalLog {
        Clock::time_point window{}; unsigned polls=0,unlocked=0; std::int32_t min_snr=0,max_snr=0;
        std::uint32_t frequency=0,layers=0; std::array<unsigned long long,3> error_ts{},total_ts{},tmcc{};
        bool decode_warned=false;
    } signal_;
    struct SignalBase {bool valid=false; std::array<std::uint32_t,3> error_ts{},total_ts{},tmcc{};} signal_base_;
    bool signal_log_=false;
    std::atomic_bool stop_{true},output_{false}; std::atomic_int error_{0};
    CaptureGate gate_;
    std::uint16_t expected_=0; bool received_=false; std::vector<std::uint8_t> response_;
    // J.83B。iq_ と loaded_ は mutex_ で保護し、iq_feed_ と iq_output_ は受信と helper の読み取りのスレッドが読む。
    // loaded_ の Unknown は、この DriverHost が読み込んでいない (open 前から稼働中の) firmware
    enum class Loaded {Unknown,Isdbt,J83b};
    J83bConfig j83b_; bool j83b_supported_=false,iq_=false; Loaded loaded_=Loaded::Unknown;
    std::atomic_bool iq_feed_{false},iq_output_{false};
    // firmware の読み込み直しの間か (Io::Reloading) と、その間に回復させた受信の失敗の数
    std::atomic_bool reloading_{false}; std::atomic_uint reload_recoveries_{0};
    // IQ の送出中に観測した、未完了の受信要求の最小数と、完了から出し直すまでの最大時間 (受信のスレッドが書く)
    std::atomic_uint iq_min_pending_{0}; std::atomic<std::uint64_t> iq_max_resubmit_us_{0};
    IqHelper iq_helper_;
    std::thread worker_;
};

Device::Device(const std::wstring& path,const DeviceDefinition& definition,std::uintptr_t index,ReceiverManager& manager)
    : DeviceBase(path,definition,index,manager,true),index_(index)
{
    removed_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!removed_) throw DeviceError("PX-S1UD: CreateEventW failed");
}
Device::~Device() {SetAvailability(false);Term();if (removed_) CloseHandle(removed_);}
int Device::Init()
{
    if (usb_dev_.descriptor.idVendor!=0x3275 || usb_dev_.descriptor.idProduct!=0x0080) return -ENODEV;
    // J.83B は INI の System に書き、J83BFirmwarePath を指定した場合だけ受け付ける
    if (device_def_.receivers.size()!=1) return -EINVAL;
    const auto systems=device_def_.receivers[0].systems;
    const bool j83b=(systems&SystemType::J83B)==SystemType::J83B;
    if (systems!=SystemType::ISDB_T && systems!=(SystemType::ISDB_T|SystemType::J83B)) return -EINVAL;
    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (!WinUsb_QueryInterfaceSettings(usb_dev_.winusb,0,&descriptor)) return -EIO;
    UCHAR in=0; USHORT packet=0; bool out=false;
    for (UCHAR i=0;i<descriptor.bNumEndpoints;++i) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(usb_dev_.winusb,0,i,&pipe)) return -EIO;
        if (pipe.PipeType!=UsbdPipeTypeBulk) continue;
        if (pipe.PipeId&0x80) {if (in) return -ENOSYS;in=pipe.PipeId;packet=pipe.MaximumPacketSize;}
        else if (pipe.PipeId==2) out=true;
    }
    if (!in || !out || packet<=SMS_HEADER_SIZE) return -ENOSYS;
    // RAW_IO は使わず、短い応答で要求を完了させ、過剰分を次要求へ残す。
    BOOL disabled=FALSE;
    if (!WinUsb_SetPipePolicy(usb_dev_.winusb,in,RAW_IO,sizeof(disabled),&disabled) ||
        !WinUsb_SetPipePolicy(usb_dev_.winusb,in,IGNORE_SHORT_PACKETS,sizeof(disabled),&disabled)) return -EIO;
    const auto directory=HostDirectory();
    const std::filesystem::path firmware=device_def_.configs.Get(L"FirmwarePath",L"");
    Receiver::J83bConfig j83b_config{};
    if (j83b) {
        const auto absolute=[&](std::filesystem::path p) {return (p.is_relative() ? directory/p : p).lexically_normal();};
        const std::filesystem::path j83b_firmware=device_def_.configs.Get(L"J83BFirmwarePath",L"");
        if (j83b_firmware.empty()) {msg_err("PX-S1UD: System includes J.83B but J83BFirmwarePath is not set\n");return -EINVAL;}
        j83b_config.firmware=absolute(j83b_firmware);
        j83b_config.helper=absolute(device_def_.configs.Get(L"HelperPath",L"msi2500\\msi2500_frontend.exe"));
        // ログ名は DriverHost 内の機器番号で区別する (MSi2500 と同じ)
        j83b_config.log=directory/(L"px-s1ud_j83b-"+std::to_wstring(index_)+L".log");
    }
    receiver_=std::make_unique<Receiver>(usb_dev_.dev,usb_dev_.winusb,removed_,in,packet-SMS_HEADER_SIZE,directory,firmware,
        j83b ? &j83b_config : nullptr);
    const auto& def=device_def_.receivers[0];command::ReceiverInfo info{};
    wcscpy_s(info.device_name,device_def_.name.c_str());info.device_guid=device_def_.guid;
    wcscpy_s(info.receiver_name,def.name.c_str());info.receiver_guid=def.guid;
    info.systems=systems;info.index=def.index;
    if (!receiver_manager_.Register(info,receiver_.get(),GetDeviceIdentity())) {receiver_.reset();return -EIO;}
    return 0;
}
void Device::Term()
{
    SetAvailability(false);
    if (receiver_) {receiver_manager_.Unregister(receiver_.get());receiver_.reset();}
}
void Device::SetAvailability(bool available)
{
    if (!available) {SetEvent(removed_);if (receiver_) receiver_->Disconnected();}
}
ReceiverBase* Device::GetReceiver(int id) const
{
    if (id!=0) throw std::out_of_range("PX-S1UD receiver index");return receiver_.get();
}
}
