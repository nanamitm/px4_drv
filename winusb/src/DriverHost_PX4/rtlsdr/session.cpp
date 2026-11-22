// SPDX-License-Identifier: GPL-2.0-only
#include "session.hpp"
#include "../pxbcud/recovery_budget.hpp"
#include "msg.h"
#include <array>
#include <cerrno>
#include <exception>

namespace px4::rtlsdr {
std::uint64_t NowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool HelperFrequency(std::uint32_t khz, std::uint32_t &hz)
{
    if (khz < 473143 || khz > 707143 || (khz - 473143) % 6000) return false;
    hz = 473142857 + ((khz - 473143) / 6000) * 6000000;
    return true;
}

bool Session::Locked() const
{
    const auto last = last_ts_.load();
    return ready_ && last && NowMs() - last < LockWindowMs;
}

std::uint64_t Session::RestartCount()
{
    std::lock_guard<std::mutex> handles(handle_mutex_);
    return restart_count_;
}

int Session::Start(std::uint32_t hz)
{
    Stop();
    {
        std::lock_guard<std::mutex> handles(handle_mutex_);
        drain_only_ = false;
        hz_ = hz;
        const int result = port_.Launch(hz, restart_count_ != 0);
        if (result) {
            drain_only_ = true;
            return result;
        }
    }
    stop_ = false;
    exhausted_ = false;
    ClearStatus();
    ++run_;
    try {
        worker_ = std::thread([this] { Supervise(); });
    } catch (const std::exception &error) {
        msg_err("RTL-SDR: launch failed: %s\n", error.what());
        Stop();
        return -EIO;
    }
    return 0;
}

void Session::Stop()
{
    // 停止要求後も stdout を排出する。先に reader を止めると、パイプへ書き込み中の
    // helper が詰まり、stdin の停止要求を受けても終了できなくなる。
    // drain_only_ を立てた後は監視のスレッドが資源を入れ替えないため、ロックの外で待てる
    {
        std::lock_guard<std::mutex> handles(handle_mutex_);
        drain_only_ = true;
        port_.RequestStop();
    }
    port_.WaitStopped(std::chrono::milliseconds(3000));
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> handles(handle_mutex_);
        port_.Release();
    }
    ClearStatus();
}

/*
 * helper が予期せず終わったとき (USB の I/Q が止まる、異常終了、応答なし、形式不正)、同じ周波数で helper を
 * 起動し直す。配信の接続と世代は保ち、欠けた区間は配信先が continuity counter で検出する。打ち切ると
 * 選局し直すまで録画が止まるため、PX-BCUD の自動復旧と同じ回数の制限と待機で続ける。
 * この helper はどの失敗でも終了コード 1 を返す。引数は起動前に確かめているため、
 * 終了コードでは区別せず、回数の制限で止める。回数を使い切った後も配信の接続は残し、
 * ロックの失敗で知らせて選局し直しを待つ。
 */
void Session::Supervise() noexcept
{
    px4::pxbcud::RecoveryBudget budget;
    for (;;) {
        const auto run = ReadOutput();
        if (Stopping()) break;
        std::uint32_t exit_code = NoExitCode;
        {
            std::lock_guard<std::mutex> handles(handle_mutex_);
            if (Stopping()) break;
            // 形式不正や応答なしでは helper がまだ動いているため、強制終了してから回収する
            exit_code = port_.Kill();
        }
        ready_ = false; quality_ = 0; mer_x100_ = -1;
        const auto failed_at = NowMs();
        budget.NoteStream(std::chrono::milliseconds(run.received_ms));
        bool started = false;
        while (!started) {
            if (!budget.Take()) {
                exhausted_ = true; quality_ = 0;
                msg_err("RTL-SDR: auto restart exhausted exit_code=%lu restart_count=%llu limit=%u; waiting for a retune\n",
                    static_cast<unsigned long>(exit_code), static_cast<unsigned long long>(RestartCount()),
                    static_cast<unsigned>(px4::pxbcud::RecoveryBudget::Limit));
                return;
            }
            const auto backoff = budget.Backoff();
            for (auto waited = std::chrono::milliseconds::zero(); waited < backoff && !Stopping(); waited += std::chrono::milliseconds(10))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            /*
             * 抜去では helper が USB の失敗で先に終わり、抜去の通知はその後に届くことがある (MSi2500 の実機で確認)。
             * 通知の前に起動し直すと、抜いた機器へ helper を起動して失敗させるため、機器が見えなければ最長 3 秒待つ。
             * 期限を過ぎても見えないときは起動し直しの失敗として数える
             */
            if (!port_.DevicePresent()) {
                for (int i = 0; i < 300 && !Stopping() && !port_.DevicePresent(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (Stopping()) return;
                if (!port_.DevicePresent()) { msg_warn("RTL-SDR: device interface is missing; not restarting the helper\n"); continue; }
            }
            std::lock_guard<std::mutex> handles(handle_mutex_);
            if (Stopping()) return;
            ++restart_count_;
            msg_warn("RTL-SDR: helper restart count=%llu exit_code=%lu consecutive=%u backoff_ms=%lld possible_ts_loss=1\n",
                static_cast<unsigned long long>(restart_count_), static_cast<unsigned long>(exit_code),
                static_cast<unsigned>(budget.Consecutive()), static_cast<long long>(backoff.count()));
            started = !port_.Launch(hz_, true);
            if (!started) msg_err("RTL-SDR: helper restart failed count=%llu\n", static_cast<unsigned long long>(restart_count_));
            else ++run_;
        }
        restarted_at_ = failed_at;
    }
}

// 1 つの helper の出力を、終わるか停止を求められるまで読む。最初と最後の TS の間を返す
Session::HelperRun Session::ReadOutput() noexcept
{
    HelperRun run;
    try {
        Frames frames;
        std::array<std::uint8_t, 16384> bytes{};
        const auto run_id = run_.load();
        auto last_message = NowMs();
        std::uint64_t first_ts = 0;
        while (!stop_ && available_) {
            const int got = port_.Read(bytes.data(), bytes.size(), std::chrono::milliseconds(10));
            if (got < 0) break;
            if (!got) {
                if (NowMs() - last_message > 30000) { msg_err("RTL-SDR: helper heartbeat stalled\n"); break; }
                continue;
            }
            const bool valid = frames.Feed(bytes.data(), static_cast<std::size_t>(got), [&](const Frame &frame, const std::uint8_t *data) {
                last_message = NowMs();
                if (frame.kind == 1) ready_ = true;
                if (frame.kind == 4) {
                    msg_err("RTL-SDR: helper error: %.*s\n", static_cast<int>(frame.size), reinterpret_cast<const char *>(data));
                    return false;
                }
                if (frame.kind == 3) {
                    quality_ = frame.quality;
                    // MER は TS 出力中だけ載る。載らない状態フレームでは直前の値を残し、ロックの判定は TS の到着で行う。
                    std::uint32_t mer = 0;
                    if (StatusMer(frame, data, mer)) mer_x100_ = static_cast<std::int32_t>(mer);
                }
                if (frame.kind == 2) {
                    quality_ = frame.quality;
                    const auto now = NowMs();
                    last_ts_ = now;
                    if (!first_ts) first_ts = now;
                    run.received_ms = now - first_ts;
                    if (const auto restarted = restarted_at_.exchange(0)) {
                        msg_warn("RTL-SDR: helper restart resumed count=%llu outage_ms=%llu possible_ts_loss=1\n",
                            static_cast<unsigned long long>(RestartCount()), static_cast<unsigned long long>(now - restarted));
                    }
                    if (!stop_ && !drain_only_) sink_(data, frame.size, run_id);
                }
                return true;
            });
            if (!valid) { msg_err("RTL-SDR: invalid or failed helper frame\n"); break; }
        }
    } catch (const std::exception &error) {
        msg_err("RTL-SDR: reader failed: %s\n", error.what());
    }
    if (!Stopping()) msg_err("RTL-SDR: frontend stopped unexpectedly\n");
    return run;
}
}
