// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <sys/types.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace px4::cuse {
// RTL-SDR のワンセグの helper (rtlsdr_frontend --stdin) の子プロセス。デーモンが USB から受けた I/Q を標準入力へ渡し、
// 標準出力 (helper のフレーム) を rtlsdr::Session が Read で読む。標準エラー出力はデーモンの標準エラー出力 (journal) をそのまま使う
// (px4-cused.service は ProtectSystem=strict で、ログのファイルを書く場所を持たないため)。
// Spawn、RequestStop、Kill、Release は rtlsdr::Session のハンドルのロックの下で呼ばれ、Feed は libusb のイベントのスレッドから呼ばれる。
class RtlSdrHelperProcess final {
public:
    // 標準入力のキューのブロック数。USB の転送 (256 KiB、約 0.13 秒) を単位に約 8 秒分を持つ。
    // helper 自身も約 16.5 秒分のキューを持ち (WinUSB 版と同じ)、選局の直後の初期の復号の間も USB を止めずに済む
    static constexpr std::size_t QueueBlocks = 64;

    RtlSdrHelperProcess() = default;
    ~RtlSdrHelperProcess();
    RtlSdrHelperProcess(const RtlSdrHelperProcess &) = delete;
    RtlSdrHelperProcess &operator=(const RtlSdrHelperProcess &) = delete;

    // argv[0] は実行ファイル。新しいプロセスグループで起動する
    int Spawn(const std::vector<std::string> &argv);
    // I/Q のブロックを標準入力のキューへ積む。キューが満杯なら false を返し、以後の Read を失敗にする
    // (I/Q を捨てて復調を続けると欠落を隠すため、WinUSB 版の helper と同じく失敗として起動し直す)
    bool Feed(const std::uint8_t *data, std::size_t size);
    // 標準出力を最長 wait 待って読む。読んだ長さ、出力がなければ 0、終了・EOF・キューのあふれは負の値
    int Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait);
    // 停止の要求 (標準入力を閉じる)。キューに残った I/Q は送らない
    void RequestStop();
    // 停止を求めた helper の終了を最長 limit 待ち、期限を過ぎたらプロセスグループを止める
    void WaitStopped(std::chrono::milliseconds limit);
    // 予期せず終わった helper をプロセスグループごと止めて回収し、資源を解放する。終了コードを返す (シグナルは 128 + 番号)
    std::uint32_t Kill();
    // 停止した helper の残りの資源を解放する
    void Release();
    bool Running() const { return pid_ > 0; }

private:
    void Write() noexcept;
    void Reap(int status);
    void StopWriter();

    pid_t pid_ = -1;
    int output_ = -1;
    std::uint32_t exit_code_ = 259;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable queued_;
    // input_ は書き込みのスレッドが閉じるまで使う。mutex_ で守る
    int input_ = -1;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::vector<std::vector<std::uint8_t>> spare_;
    bool closing_ = false;
    std::atomic_bool overflow_{false};
};
}
