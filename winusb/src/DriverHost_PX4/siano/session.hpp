// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// PX-S1UD の 1 台分の制御を、WinUSB 版 (DriverHost_PX4) と Linux 版 (px4-cused) で共有する。
// 地デジと J.83B の firmware の切り替えの順序、Bulk IN のメッセージの振り分け、要求と応答の対応、
// J.83B の helper との受け渡し、受信品質のログは、Windows の実機で見つけた条件を含み、OS ごとに複製すると
// 片方だけが修正されるため、USB の転送と待機 (SessionPort) と helper のプロセス (IqProcess) 以外をここへ集める。
// 設計は docs/20261122_px_s1ud_linux_cuse_design.md を参照。
#include "capture_gate.hpp"
#include "frontend.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace px4::siano {
using Clock=std::chrono::steady_clock;
// helper の TS の到着時刻などに使う、単調増加のミリ秒 (0 は「まだない」)
std::uint64_t MonotonicMs();

// J.83B の IQ を復調する helper (msi2500_frontend serve-s1ud) のプロセス。起動と停止とパイプは OS ごとに実装し、
// 出力の解釈は HelperOutput (iq_helper_output.hpp) を共有する
class IqProcess {
public:
    // helper の TS を受け取る。false を返すと helper を失敗として止める
    using TsHandler=std::function<bool(const std::uint8_t*,std::size_t)>;
    virtual ~IqProcess()=default;
    virtual int Start(TsHandler ts)=0;
    // 受信のスレッドから呼ぶ。待たずに複製してキューへ積み、満杯なら捨てて数える
    virtual void Feed(const std::uint8_t* block,std::size_t size)=0;
    // 標準入力を閉じて helper の終了を待つ (期限を過ぎたら強制終了する)
    virtual void Stop()=0;
    virtual bool Running() const=0;
    virtual bool Ready() const=0;
    virtual bool Failed() const=0;
    // 最後の TS の時刻 (MonotonicMs)。0 はまだない
    virtual std::uint64_t LastTs() const=0;
    virtual std::uint32_t Quality() const=0;
    // 復調器の MER (dB×100)。まだ報告がないときは -1
    virtual std::int32_t MerX100() const=0;
    virtual std::uint64_t Dropped() const=0;
};

// USB の転送と待機、配信のバッファー、firmware のファイルの読み込みを OS ごとに受け持つ。
class SessionPort {
public:
    enum class FirmwareKind {Isdbt,J83b};
    virtual ~SessionPort()=default;
    // Bulk OUT へ 1 メッセージを送る。deadline を過ぎたら -ETIMEDOUT、抜去で -ECANCELED
    virtual int Send(const std::uint8_t* data,std::size_t size,Clock::time_point deadline)=0;
    virtual bool Removed() const=0;
    // 抜去で -ECANCELED を返して中断する
    virtual int Delay(unsigned milliseconds)=0;
    // Bulk IN の受信を始める (Session::Receive が呼ばれ始める)。開く処理の最初に呼ぶ
    virtual int StartReceive()=0;
    // Bulk IN の受信を止め、未完了の転送を回収する
    virtual void StopReceive()=0;
    // 受信の失敗を、配信を読んでいる利用者へ伝える
    virtual void Failed(int error)=0;
    // 選局のたびに配信の世代を進める。前の世代の TS を利用者へ渡さないため
    virtual bool AdvanceEpoch()=0;
    // firmware を読む。地デジの firmware がない場合は -ENOENT (地デジで稼働中の機器だけ開ける)
    virtual int ReadFirmware(FirmwareKind kind,std::vector<std::uint8_t>& firmware)=0;
    // J.83B の helper の TS を配信へ書く関数。helper の起動ごとに呼び、その起動の間だけ使う
    virtual std::function<void(const std::uint8_t*,std::size_t)> HelperWriter()=0;
    // 未完了の (ホストコントローラーに残っている) 受信要求の数。IQ の欠落の切り分けにだけ使う
    virtual unsigned PendingReads()=0;
    // 受信品質をログへ記録するか
    virtual bool SignalLogEnabled()=0;
};

// 受信のスレッドが 1 回の転送の完了ごとに受け取る結果
struct Received {
    // 0 以外は受信を止める失敗 (形式の異常、配信のあふれ)
    int error=0;
    // メッセージを解釈した (false は空の転送か、前のホストの残りとして読み飛ばした転送)
    bool parsed=false;
    sms_frame frame{};
};

class Session final : private Io {
public:
    enum class System {Isdbt,J83b};
    // transfers は受信要求の本数。読み込み直しの間に回復させる受信の失敗の上限 (4 倍) に使う。
    // helper が nullptr なら J.83B を受け付けない
    Session(SessionPort& port,IqProcess* helper,unsigned transfers)
        : port_(port),helper_(helper),transfers_(transfers),frontend_(*this) {}
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;

    // 制御の操作 (Open から CheckLock まで) は、呼び出し側が 1 つのロックで順に呼ぶ。
    // Open は受信を始めてから firmware を確かめ、失敗したら受信を止める
    int Open();
    // IQ の送出と helper を止め、J.83B 用の firmware なら通常の firmware へ戻す。受信は止めない。
    // 呼び出し側は、続けて配信を止めてから SessionPort::StopReceive を呼ぶ
    void Close();
    bool IsOpen() const {return open_;}
    // 配信の開始と停止。開始は配信のバッファーを用意した後、停止はバッファーを止める前に呼ぶ
    int SetCapture(bool capture);
    // 選局 (地デジは ChSet の kHz、J.83B は中心周波数の kHz)
    int Tune(System system,std::uint32_t frequency_khz);
    bool Tuned() const {return tuned_;}
    // ロックと C/N (dB×1000)。地デジは統計を 100 ms に 1 回だけ読む
    int CheckLock(bool& locked,std::int32_t& cnr);
    bool J83bSupported() const {return helper_!=nullptr;}
    std::uint16_t Version() const {return frontend_.Version();}
    int Error() const {return error_.load();}

    // 受信のスレッドから呼ぶ
    // 受信要求を出すときの世代。完了した転送の TS は、この世代が現在と一致する場合だけ配信する
    std::uint64_t Ticket() const {return gate_.Ticket();}
    // firmware の読み込み直しの間に失敗した受信を、出し直して回復させてよいか (回数を数える)
    bool ReloadRecovery();
    // 受信の失敗。この open を失敗状態にして、待機中の Exchange と配信の利用者へ伝える
    void Fail(int error);
    // 抜去を検出したとき、待機中の Exchange を起こす (Exchange は SessionPort::Removed を見て戻る)
    void Interrupt()
    {
        std::lock_guard<std::mutex> lock(response_mutex_);
        responses_changed_.notify_all();
    }
    // 完了した 1 回の転送を解釈して振り分ける。write は地デジの TS (188 バイトの倍数) を受け取り、
    // false を返すと配信のあふれとして受信を止める (CaptureGate の Feed と同じ契約)
    template<class Writer>
    Received Receive(std::uint64_t ticket,const std::uint8_t* data,std::size_t size,std::size_t alignment,Writer write)
    {
        Received r=Parse(data,size,alignment);
        if (r.error || !r.parsed) return r;
        const auto* p=data+r.frame.payload_offset;
        if (r.frame.type==MSG_SMS_DVBT_BDA_DATA) {
            if (!gate_.Feed(ticket,p,r.frame.payload_length,write)) {r.error=-ENOBUFS;Fail(r.error);}
        } else {
            Deliver(r.frame.type,p,r.frame.payload_length);
        }
        return r;
    }
    // 受信の開始ごとに、前のホストの残りの読み飛ばしと形式の異常の計数を戻す (StartReceive の中で呼ぶ)
    void ResetReceive() {synchronized_=false;stale_=0;malformed_=0;iq_malformed_=0;iq_blocks_seen_=0;}
    // 地デジの TS の境界の計数 (ログ用)。CaptureGate::Diagnostics と同じ
    CaptureGate::Counters Diagnostics() {return gate_.Diagnostics();}
    // Receive の write の中からだけ呼ぶ
    CaptureGate::Counters DiagnosticsInWrite() const noexcept {return gate_.DiagnosticsInWrite();}
    // 受信の終わりに記録する、形式の異常の数
    std::uint64_t Malformed() const {return malformed_;}
    std::uint64_t IqMalformed() const {return iq_malformed_;}

private:
    int Exchange(const std::vector<std::uint8_t>& message,std::uint16_t response,
                 std::vector<std::uint8_t>& payload,unsigned ms) override;
    int Delay(unsigned ms) override {return port_.Delay(ms);}
    void Reloading(bool on) override;
    Received Parse(const std::uint8_t* data,std::size_t size,std::size_t alignment);
    void Deliver(std::uint16_t type,const std::uint8_t* payload,std::size_t size);
    int ReloadIsdbt();
    int TuneJ83b(std::uint32_t frequency_khz);
    int StartIqHelper();
    int StopIq();
    void LogSignal(const std::vector<std::uint8_t>& p);
    void DisableOutput() {gate_.SetEnabled(false);output_=false;}
    void EnableOutput() {gate_.SetEnabled(true);output_=true;}

    SessionPort& port_;
    IqProcess* const helper_;
    const unsigned transfers_;
    Frontend frontend_;
    CaptureGate gate_;
    IqOutputGate iq_output_;
    // 制御の状態。呼び出し側のロックで保護する
    bool open_=false,capture_=false,tuned_=false,locked_=false,signal_log_=false;
    std::int32_t cnr_=0; Clock::time_point last_stats_{};
    struct SignalLog {
        Clock::time_point window{}; unsigned polls=0,unlocked=0; std::int32_t min_snr=0,max_snr=0;
        std::uint32_t frequency=0,layers=0; std::array<unsigned long long,3> error_ts{},total_ts{},tmcc{};
        bool decode_warned=false;
    } signal_;
    struct SignalBase {bool valid=false; std::array<std::uint32_t,3> error_ts{},total_ts{},tmcc{};} signal_base_;
    // J.83B。iq_ と loaded_ は制御の状態、iq_feed_ は受信のスレッドが読む。
    // loaded_ の Unknown は、このホストが読み込んでいない (開く前から稼働中の) firmware
    enum class Loaded {Unknown,Isdbt,J83b};
    bool iq_=false; Loaded loaded_=Loaded::Unknown;
    std::atomic_bool iq_feed_{false},output_{false};
    std::atomic_int error_{0};
    // 要求と応答の対応。応答は受信のスレッドが置く
    std::mutex response_mutex_; std::condition_variable responses_changed_;
    std::uint16_t expected_=0; bool received_=false; std::vector<std::uint8_t> response_;
    // 受信の開始後に最初のメッセージを解釈したか、それまでに読み飛ばした転送の数 (受信のスレッドだけが使う)
    bool synchronized_=false; unsigned stale_=0;
    std::atomic<std::uint64_t> malformed_{0},iq_malformed_{0};
    // firmware の読み込み直しの間か (Io::Reloading) と、その間に回復させた受信の失敗の数
    std::atomic_bool reloading_{false}; std::atomic_uint reload_recoveries_{0};
    // IQ の送出中に観測した、未完了の受信要求の最小数と、IQ の処理の間隔の最大 (受信のスレッドが書く)
    std::atomic_uint iq_min_pending_{0}; std::atomic<std::uint64_t> iq_max_resubmit_us_{0};
    std::uint64_t iq_blocks_seen_=0; Clock::time_point last_iq_{};
};
}
