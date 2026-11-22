// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// RTL-SDR のワンセグの helper の監視。WinUSB 版 (rtlsdr/rtl_device.cpp) と Linux 版 (cuse/src/rtlsdr_device.cpp) で共有する。
// helper のプロセスと USB の扱いは OS ごとに異なるため HelperPort で受け取り、
// 起動し直しの条件と回数、helper の出力の解釈、ロックの判定をここに置く (docs/20261122_rtlsdr_linux_cuse_design.md)。
#include "protocol.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace px4::rtlsdr {
// TS の到着時刻とロックの判定に使う単調な時刻 (ms)
std::uint64_t NowMs();

// PTX と BonDriver の地デジの周波数 (kHz、473,143 + 6,000 × (チャンネル − 13)) を、helper へ渡す中心周波数 (Hz) へ換算する。
// 復調は 1/7 MHz のずれを含む UHF 13〜52ch の格子だけを前提とするため、それ以外は false を返す
bool HelperFrequency(std::uint32_t khz, std::uint32_t &hz);

// helper のプロセス (Linux 版では USB の受信を含む) の OS ごとの操作。
// Launch、RequestStop、Kill、Release は Session のハンドルのロックを保持して呼ばれる。
// Read は監視のスレッドだけが、WaitStopped は停止の要求の後に、どちらもロックの外で呼ぶ。
class HelperPort {
public:
    virtual ~HelperPort() = default;
    // helper を起動する。restarted は起動し直しのとき true (ログを追記で開く)。失敗した場合は作りかけの資源を自分で閉じる
    virtual int Launch(std::uint32_t hz, bool restarted) = 0;
    // helper の標準出力を読む。読んだ長さ、wait の間に出力のない場合は 0、終了・EOF・失敗は負の値
    virtual int Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait) = 0;
    // 停止を求める (標準入力を閉じる)
    virtual void RequestStop() = 0;
    // 停止を求めた helper の終了を最長 limit 待ち、期限を過ぎたら強制終了する
    virtual void WaitStopped(std::chrono::milliseconds limit) = 0;
    // 予期せず終わった helper を強制終了して回収し、資源を解放して終了コードを返す
    virtual std::uint32_t Kill() = 0;
    // 停止した helper の残りの資源を解放する
    virtual void Release() = 0;
    // 機器がまだあるか。起動し直す前に確かめる
    virtual bool DevicePresent() = 0;
};

class Session final {
public:
    // run は helper の起動ごとに増える番号 (配信の溢れの記録を起動ごとに分けるため)
    using TsSink = std::function<void(const std::uint8_t *, std::size_t, std::uint64_t run)>;
    // 終了コードを取れない場合の値 (Windows の STILL_ACTIVE と同じ)
    static constexpr std::uint32_t NoExitCode = 259;
    // TS がこの時間内に届いていればロックとみなす
    static constexpr std::uint64_t LockWindowMs = 3000;

    Session(HelperPort &port, TsSink sink) : port_(port), sink_(std::move(sink)) {}
    ~Session() { Stop(); }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // 前の helper を止めてから、hz で helper を起動して監視を始める
    int Start(std::uint32_t hz);
    // helper を止める。何度呼んでもよい
    void Stop();
    // 機器の抜去。起動し直しを止め、読み取りを終わらせる (helper の回収は Stop で行う)
    void Disconnect() { available_ = false; stop_ = true; quality_ = 0; }

    bool Locked() const;
    // 起動し直しの回数を使い切った。次の Start まで続く
    bool Exhausted() const { return exhausted_; }
    bool Ready() const { return ready_; }
    std::uint64_t LastTs() const { return last_ts_; }
    std::uint32_t Quality() const { return quality_; }
    std::int32_t MerX100() const { return mer_x100_; }
    std::uint64_t RestartCount();

private:
    struct HelperRun {
        std::uint64_t received_ms = 0;
    };
    // 起動し直しを止めるべきか。Close・選局・取り込みの停止 (drain_only_) と抜去 (available_) を優先する
    bool Stopping() const { return stop_ || drain_only_ || !available_; }
    void Supervise() noexcept;
    HelperRun ReadOutput() noexcept;
    void ClearStatus() { ready_ = false; last_ts_ = 0; quality_ = 0; mer_x100_ = -1; }

    HelperPort &port_;
    const TsSink sink_;
    std::uint32_t hz_ = 0;
    std::atomic_bool available_{true}, stop_{true}, drain_only_{true}, exhausted_{false}, ready_{false};
    std::atomic<std::uint32_t> quality_{0};
    std::atomic<std::int32_t> mer_x100_{-1};
    std::atomic<std::uint64_t> last_ts_{0};
    // helper の資源。監視のスレッドが起動し直すときに入れ替えるため、handle_mutex_ で守る。
    // 呼び出し側のロック (WinUSB 版の Receiver::mutex_ など) の後に取り、監視のスレッドは handle_mutex_ だけを取る
    std::mutex handle_mutex_;
    // 起動し直した回数 (handle_mutex_ を保持して更新する) と、起動し直した後に最初の TS を待つ間の失敗の時刻
    std::uint64_t restart_count_ = 0;
    std::atomic<std::uint64_t> restarted_at_{0}, run_{0};
    std::thread worker_;
};
}
