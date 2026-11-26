// SPDX-License-Identifier: GPL-2.0-only
#include "session.hpp"
#include "msg.h"
#include <algorithm>
#include <cstdio>

namespace px4::siano {
namespace {
// 受信の開始時に読み飛ばす、前のホストの残りの転送の上限。実機の残りは 1 回の転送だった。
// 上限を超えても解釈できない場合は、残りではなく形式の異常として受信を止める
constexpr unsigned MaxStaleTransfers=4;
}

std::uint64_t MonotonicMs()
{
    // 0 を「まだない」に使うため、起動直後でも 0 にならないよう 1 を足す
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count())+1;
}

int Session::Open()
{
    if (open_) return -EBUSY;
    if (port_.Removed()) return -ECANCELED;
    // 配置変更後の再オープンでも、firmware の配置先を探し直す
    std::vector<std::uint8_t> firmware;
    const int read=port_.ReadFirmware(SessionPort::FirmwareKind::Isdbt,firmware);
    if (read && read!=-ENOENT) return read;
    if (read) msg_warn("PX-S1UD: firmware not found; only an already initialized device can open\n");
    error_=0; ResetReceive();
    int ret=port_.StartReceive();
    if (ret) return ret;
    ret=frontend_.Initialize(firmware);
    if (ret==-EIO && !firmware.empty()) {
        // J.83B 用の firmware が稼働したままの機器 (このホストや BonDriver_S1UD.dll が読み込んだもの) は、
        // ISDB-T の INIT_DEVICE を受けても ISDB-T のモードへ移らない。通常の firmware を読み込み直す
        msg_warn("PX-S1UD: device did not enter ISDB-T mode; reloading the ISDB-T firmware\n");
        ret=frontend_.Initialize(firmware,true);
    }
    if (ret) {
        msg_err("PX-S1UD: initialization failed ret=%d\n",ret);
        port_.StopReceive(); return ret;
    }
    open_=true; tuned_=false; capture_=false; locked_=false; cnr_=0;
    // Initialize は ISDB-T のモードを確かめてから成功を返す。前の open が失敗状態のまま閉じると、Close は
    // 通常の firmware への読み込み直しを省き、loaded_ に J.83B が残る。そのままでは J.83B の選局が
    // J.83B 用の firmware を読み込まずに IQ の送出を要求し、通常の firmware が応答せずに失敗する
    loaded_=Loaded::Isdbt;
    signal_log_=port_.SignalLogEnabled(); signal_={}; signal_base_={};
    msg_info("PX-S1UD: opened firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
    return 0;
}

void Session::Close()
{
    if (!open_) return;
    capture_=false; tuned_=false; DisableOutput();
    // IQ の送出を止めてから受信を止める。送出を止めずに閉じると、次の open で IQ のメッセージが
    // 制御応答と同じ bulk IN を埋め続ける
    StopIq();
    // J.83B 用の firmware のまま閉じると、受信を止めて開き直した後の GET_VERSION に応答しなかった。
    // 他のホストやアプリも ISDB-T の firmware を前提に開くため、受信を動かしている間に通常の firmware へ戻す
    if (loaded_==Loaded::J83b && !error_ && !port_.Removed() && ReloadIsdbt())
        msg_warn("PX-S1UD: closed with the J.83B firmware still running\n");
    open_=false;
}

int Session::SetCapture(bool capture)
{
    if (!open_) return -EINVAL;
    if (port_.Removed()) return -ECANCELED;
    if (capture==capture_) return capture ? -EALREADY : 0;
    if (capture) {
        capture_=true;
        if (iq_) iq_output_.SetEnabled(true);
        else if (tuned_ && locked_) EnableOutput();
    } else {
        capture_=false; DisableOutput(); iq_output_.SetEnabled(false);
    }
    return error_.load();
}

int Session::CheckLock(bool& locked,std::int32_t& cnr)
{
    locked=false; cnr=0;
    if (!open_) return -EINVAL;
    if (port_.Removed()) return -ECANCELED;
    if (error_) return error_;
    if (!tuned_) return 0;
    if (iq_) {
        // J.83B の復調は helper が行い、firmware の統計は ISDB-T のもののため使わない。
        // MSi2500 と同じく、helper の TS が 3 秒以内に届いていればロックとする。
        // C/N には helper が報告する復調器の MER (dB) を ISDB-T の CNR と同じ dB×1000 で返す。
        // RS の成功割合は正常受信中にほぼ 100% で変わらず、C/N の目安にならないため。
        // MER をまだ報告していない間 (起動直後と MER を送らない古い helper) は、従来どおり RS の成功割合を返す
        // helperだけの異常終了では配信接続を閉じず、次の選局でhelperを作り直せるようにする。
        if (helper_->Failed()) return -ECANCELED;
        const auto last=helper_->LastTs();
        locked_=helper_->Ready() && last && MonotonicMs()-last<3000;
        const auto mer=helper_->MerX100();
        cnr_=!locked_ ? 0 : mer>=0 ? mer*10 : static_cast<std::int32_t>(helper_->Quality()*10);
        locked=locked_; cnr=cnr_;
        return 0;
    }
    const auto now=Clock::now();
    if (now-last_stats_>=std::chrono::milliseconds(100)) {
        std::vector<std::uint8_t> p;
        int ret=frontend_.RequestStats(p);
        if (!ret) ret=frontend_.DecodeStats(p,locked_,cnr_);
        if (ret) {
            Fail(ret);
            msg_err("PX-S1UD: statistics failed ret=%d\n",ret);
            return ret;
        }
        if (signal_log_) LogSignal(p);
        last_stats_=Clock::now();
    }
    locked=locked_; cnr=locked ? cnr_ : 0;
    if (locked && capture_ && !output_) EnableOutput();
    return 0;
}

int Session::Tune(System system,std::uint32_t frequency_khz)
{
    const bool j83b=system==System::J83b;
    if (!open_) return -EINVAL;
    // J.83B はチャンネル定義の中心周波数 (kHz) をそのまま使う (MSi2500 と同じ範囲)
    if (j83b && (!helper_ || frequency_khz<90000 || frequency_khz>775000)) return -EINVAL;
    if (port_.Removed()) return -ECANCELED;
    if (error_) return error_;
    if (!j83b && !Frontend::IsdbtFrequencySupported(frequency_khz)) return -EINVAL;
    DisableOutput(); tuned_=false; locked_=false; cnr_=0;
    // 前のチャンネルの IQ の送出と helper を止めてから世代を切り替え、前の TS と復調状態を次へ持ち込まない
    int ret=StopIq();
    // Siano の制御応答と TS は同じ bulk IN を通る。選局ごとの取消・flush は
    // メッセージ途中の USB パケットを残し、再開後に TS の端数をヘッダーとして
    // 読むことがある。受信要求を継続し、Gate の世代で選局前とロック前の TS を除く。
    if (!ret && !port_.AdvanceEpoch()) return -EIO;
    // helperの起動失敗はUSB通信の異常ではない。ファームウェアを切り替える前に起動し、
    // 失敗時も同じ接続で次の選局を受け付ける。
    if (!ret && j83b) {
        if (const int helper_error=StartIqHelper()) return helper_error;
    }
    if (!ret) {
        if (j83b) ret=TuneJ83b(frequency_khz);
        else {
            if (loaded_==Loaded::J83b) ret=ReloadIsdbt();
            if (!ret) ret=frontend_.Tune(frequency_khz);
        }
    }
    if (ret) {
        msg_err("PX-S1UD: tune failed system=%s frequency_khz=%u ret=%d\n",j83b ? "J.83B" : "ISDB-T",frequency_khz,ret);
        // 失敗した後の要求と応答の対応は保証できないため、この open を失敗状態にする
        StopIq();
        Fail(ret);
        return ret;
    }
    tuned_=true; last_stats_=Clock::time_point{}; signal_={}; signal_base_={};
    return 0;
}

// J.83B 用の firmware から通常の firmware へ読み込み直す
int Session::ReloadIsdbt()
{
    std::vector<std::uint8_t> firmware;
    int ret=port_.ReadFirmware(SessionPort::FirmwareKind::Isdbt,firmware);
    if (!ret) ret=frontend_.Initialize(firmware,true);
    if (ret) {msg_err("PX-S1UD: ISDB-T firmware reload failed ret=%d\n",ret);return ret;}
    loaded_=Loaded::Isdbt;
    msg_info("PX-S1UD: ISDB-T firmware reloaded firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
    return 0;
}

int Session::StartIqHelper()
{
    // helper を先に起動し、IQ の送出の始めのブロックから渡す
    // helper の読み取りスレッドだけが呼ぶため、書き込みの関数はこの起動の間だけ callback が持つ。
    auto writer=port_.HelperWriter();
    int ret=helper_->Start([this,writer](const std::uint8_t* data,std::size_t size) {
        iq_output_.Write([&] {writer(data,size);});
        return true;
    });
    if (ret) {msg_err("PX-S1UD: J.83B helper start failed ret=%d\n",ret);return ret;}
    return 0;
}

int Session::TuneJ83b(std::uint32_t frequency_khz)
{
    if (loaded_!=Loaded::J83b) {
        std::vector<std::uint8_t> firmware;
        int ret=port_.ReadFirmware(SessionPort::FirmwareKind::J83b,firmware);
        if (ret) {msg_err("PX-S1UD: J.83B firmware read failed ret=%d\n",ret);return ret;}
        ret=frontend_.Initialize(firmware,true,false);
        if (ret) {msg_err("PX-S1UD: J.83B firmware load failed ret=%d\n",ret);return ret;}
        loaded_=Loaded::J83b;
        msg_info("PX-S1UD: J.83B firmware loaded firmware=%u.%u\n",frontend_.Version()>>8,frontend_.Version()&255);
    }
    iq_min_pending_=transfers_; iq_max_resubmit_us_=0;
    iq_feed_=true;
    const int ret=frontend_.SetIqStream(frequency_khz*1000);
    if (ret) {
        if (ret==-ETIMEDOUT) msg_err("PX-S1UD: the firmware does not answer the IQ stream request\n");
        return ret;
    }
    iq_=true; iq_output_.SetEnabled(capture_);
    msg_info("PX-S1UD: J.83B started frequency_khz=%u\n",frequency_khz);
    return 0;
}

// IQ の送出と helper を止める。送出の停止要求の失敗は、この open の失敗として返す
int Session::StopIq()
{
    int ret=0;
    iq_output_.SetEnabled(false);
    if (iq_) {
        iq_=false;
        std::vector<std::uint8_t> p;
        if (!error_ && !port_.Removed()) ret=frontend_.SetIqStream(0,&p);
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
    if (helper_ && helper_->Running()) {
        const auto dropped=helper_->Dropped();
        helper_->Stop();
        if (dropped) msg_warn("PX-S1UD: J.83B helper queue dropped %llu blocks\n",static_cast<unsigned long long>(dropped));
    }
    return ret;
}

void Session::Reloading(bool on)
{
    if (on) reload_recoveries_=0;
    reloading_=on;
}

bool Session::ReloadRecovery()
{
    // firmware の読み込み直しで J.83B 用の firmware が ROM へ戻るとき、受信が一度失敗し、
    // 残りの要求も取り消される。読み込み直しの間だけ、要求の数の 4 倍までに限って出し直す
    if (!reloading_ || reload_recoveries_>=transfers_*4) return false;
    ++reload_recoveries_;
    return true;
}

void Session::Fail(int error)
{
    // 通し番号のない応答を次の要求へ誤対応させないため、この open を失敗状態にする。最初の原因を残す
    int expected=0;
    error_.compare_exchange_strong(expected,error);
    port_.Failed(error);
    std::lock_guard<std::mutex> lock(response_mutex_);
    responses_changed_.notify_all();
}

int Session::Exchange(const std::vector<std::uint8_t>& message,std::uint16_t response,
                      std::vector<std::uint8_t>& payload,unsigned ms)
{
    if (port_.Removed()) return -ECANCELED;
    if (error_) return error_;
    const auto deadline=Clock::now()+std::chrono::milliseconds(ms);
    {
        std::lock_guard<std::mutex> lock(response_mutex_);
        expected_=response; received_=false; response_.clear();
    }
    int ret=port_.Send(message.data(),message.size(),deadline);
    if (!ret && response) {
        std::unique_lock<std::mutex> lock(response_mutex_);
        if (!responses_changed_.wait_until(lock,deadline,[&]{return received_ || error_ || port_.Removed();})) ret=-ETIMEDOUT;
        else if (port_.Removed()) ret=-ECANCELED;
        else if (error_) ret=error_;
        else payload=response_;
        expected_=0;
    }
    if (ret) {
        Fail(ret);
        msg_err("PX-S1UD: command=%u response=%u failed ret=%d\n",sms_get_le16(message.data()),response,ret);
    }
    return ret;
}

Received Session::Parse(const std::uint8_t* data,std::size_t size,std::size_t alignment)
{
    Received r{};
    if (!size) return r;
    const auto parse=sms_frame_message(data,size,alignment,&r.frame);
    if (parse && !synchronized_ && stale_<MaxStaleTransfers) {
        // 前のホストが受信を止めたとき、メッセージの途中で取り消した読み取りの残りは機器側に残り、
        // ホストのパイプのリセットでは消えない。実機では、先頭が TS の途中の 1 回の転送だけで、
        // その後は境界から読めた。最初のメッセージを解釈するまでは、この残りを読み飛ばす
        ++stale_;
        msg_warn("PX-S1UD: skipped a stale USB transfer bytes=%zu ret=%d\n",size,parse);
        return r;
    }
    if (parse) {
        ++malformed_;r.error=-EBADMSG;Fail(r.error);
        msg_err("PX-S1UD: invalid USB frame bytes=%zu alignment=%zu ret=%d\n",size,alignment,parse);
        return r;
    }
    synchronized_=true; r.parsed=true;
    return r;
}

void Session::Deliver(std::uint16_t type,const std::uint8_t* p,std::size_t size)
{
    if (type==MSG_IQ_DATA) {
        // J.83B の IQ のブロック。復調は helper が行い、ここでは待たずにキューへ積む
        if (size!=IqBlockBytes) {++iq_malformed_;return;}
        if (!iq_feed_ || !helper_) return;
        helper_->Feed(p,size);
        // 欠落の切り分け用: 前の IQ の処理からの間隔の最大と、8 ブロックごとに数えた、
        // ホストコントローラーに残っている (未完了の) 受信要求の最小数
        const auto now=Clock::now();
        if (iq_blocks_seen_++) {
            const auto us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-last_iq_).count());
            if (us>iq_max_resubmit_us_) iq_max_resubmit_us_=us;
        }
        last_iq_=now;
        if (!(iq_blocks_seen_&7)) {
            const unsigned outstanding=port_.PendingReads();
            if (outstanding<iq_min_pending_) iq_min_pending_=outstanding;
        }
        return;
    }
    std::lock_guard<std::mutex> lock(response_mutex_);
    if (type==expected_) {
        response_.assign(p,p+size);received_=true;responses_changed_.notify_all();
    }
}

// 受信品質を TS 異常と同じ時刻軸で照合する。統計はアプリの信号レベル取得で送る既存要求を流用し、
// TS と同じ bulk IN を使う制御転送を増やさない。誤り・ロック外れの応答は即時に、
// それ以外は約10秒ごとに集計する。解釈できない応答でも受信は止めない。
// 実機の err・total・tmcc はロック時に0へ戻る累積値のため、前回応答からの増分で判定・集計する。
void Session::LogSignal(const std::vector<std::uint8_t>& p)
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
        const int n=std::snprintf(detail+used,sizeof(detail)-used," %c:seg=%u err=+%u(%u) total=+%u(%u) ber=%lld tmcc=+%u(%u)",
            static_cast<char>('A'+i),l.segments,err,l.error_ts,total,l.total_ts,
            l.ber==0xffffffff ? -1LL : static_cast<long long>(l.ber),tmcc,l.tmcc_errors);
        if (n>0) used=(std::min)(used+static_cast<std::size_t>(n),sizeof(detail)-1);
    }
    signal_base_.valid=true;
    if (bad) msg_warn("PX-S1UD: signal errors rf_locked=%d demod_locked=%d snr=%d rssi=%d%s\n",
        s.rf_locked,s.demod_locked,s.snr,s.rssi,detail);
    if (now-signal_.window<std::chrono::seconds(10)) return;
    char summary[256]={}; used=0;
    for (std::size_t i=0;i<3;++i) {
        if (!signal_.total_ts[i] && !signal_.error_ts[i] && !signal_.tmcc[i]) continue;
        const int n=std::snprintf(summary+used,sizeof(summary)-used," %c:err=%llu total=%llu tmcc=%llu",
            static_cast<char>('A'+i),signal_.error_ts[i],signal_.total_ts[i],signal_.tmcc[i]);
        if (n>0) used=(std::min)(used+static_cast<std::size_t>(n),sizeof(summary)-1);
    }
    msg_info("PX-S1UD: signal summary polls=%u unlocked=%u snr_min=%d snr_max=%d frequency=%u layers=%u%s\n",
        signal_.polls,signal_.unlocked,signal_.min_snr,signal_.max_snr,signal_.frequency,signal_.layers,summary);
    const bool warned=signal_.decode_warned; signal_={}; signal_.decode_warned=warned;
}
}
