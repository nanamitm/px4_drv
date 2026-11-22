// SPDX-License-Identifier: GPL-2.0-only
#include "iq_process.hpp"
#include "msg.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace px4::cuse {
namespace {
// 標準入力のキューのブロック数 (256 ブロックは約 0.16 秒、約 3.8 MB)。WinUSB 版の IqHelper と同じ値で、
// helper の起動直後の FFT の計画や長い欠落をつなぐ試行で一時的に読み取りが遅れても捨てずに済む長さ
constexpr std::size_t QueueBlocks = 256;
// 標準入力のパイプの容量。WinUSB 版 (16 MiB) と同じく、書き込みのスレッドが helper の一時的な遅れで止まらないようにする。
// 一般のユーザーの上限 (/proc/sys/fs/pipe-max-size、既定 1 MiB) を超える指定は失敗するため、失敗しても既定のまま続ける
constexpr int InputPipeSize = 16 << 20;
// 停止要求から helper の終了までの期限。helper はキューに残ったブロックを処理してから終わる (WinUSB 版と同じ)
constexpr auto StopDeadline = std::chrono::seconds(5);

void CloseFd(int &fd)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
}
}

int PosixIqProcess::Start(TsHandler ts)
{
    Stop();
    ts_ = std::move(ts);
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) {
        const int e = errno;
        for (int fd : {in[0], in[1], out[0], out[1]})
            if (fd >= 0)
                close(fd);
        msg_err("PX-S1UD: J.83B helper pipe failed: %s\n", std::strerror(e));
        return -e;
    }
    if (fcntl(in[1], F_SETPIPE_SZ, InputPipeSize) < 0)
        fcntl(in[1], F_SETPIPE_SZ, 1 << 20);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    // 子は標準入出力だけを受け取る。dup2 した記述子は close-on-exec が外れる
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    // デーモンは SIGPIPE を無視するが、無視の設定は exec を越えて子へ残るため既定へ戻す。
    // 新しいプロセスグループで起動し、停止の期限を過ぎたらグループごと止める
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    char *const argv[] = {const_cast<char *>(executable_.c_str()), const_cast<char *>("serve-s1ud"), nullptr};
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, executable_.c_str(), &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    // 子への書込端を親が残すと、子の終了を EOF として検出できなくなる
    close(in[0]);
    close(out[1]);
    if (rc) {
        close(in[1]);
        close(out[0]);
        msg_err("PX-S1UD: J.83B helper start failed executable=%s: %s\n", executable_.c_str(), std::strerror(rc));
        return -rc;
    }
    pid_ = pid;
    input_ = in[1];
    output_ = out[0];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = false;
        queue_.clear();
    }
    output_state_.Reset();
    failed_ = false;
    stopping_ = false;
    dropped_ = 0;
    try {
        writer_ = std::thread([this] { Write(); });
        reader_ = std::thread([this] { Read(); });
    } catch (...) {
        Stop();
        return -ENOMEM;
    }
    running_ = true;
    msg_info("PX-S1UD: J.83B helper started pid=%d\n", static_cast<int>(pid));
    return 0;
}

void PosixIqProcess::Feed(const std::uint8_t *block, std::size_t size)
{
    if (!running_ || failed_)
        return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closing_)
        return;
    if (queue_.size() >= QueueBlocks) {
        ++dropped_;
        return;
    }
    // ブロックごとの確保を避けるため、書き終えたバッファを使い回す
    std::vector<std::uint8_t> buffer;
    if (!spare_.empty()) {
        buffer = std::move(spare_.back());
        spare_.pop_back();
    }
    buffer.assign(block, block + size);
    queue_.push_back(std::move(buffer));
    lock.unlock();
    queued_.notify_one();
}

void PosixIqProcess::CloseInput()
{
    std::lock_guard<std::mutex> lock(mutex_);
    CloseFd(input_);
}

void PosixIqProcess::Write() noexcept
{
    for (;;) {
        std::vector<std::uint8_t> buffer;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            // 停止要求では残りを送らずに標準入力を閉じる (選局の切り替えを待たせない)
            if (closing_)
                break;
            buffer = std::move(queue_.front());
            queue_.pop_front();
        }
        bool ok = true;
        for (std::size_t done = 0; ok && done < buffer.size();) {
            const ssize_t n = write(input_, buffer.data() + done, buffer.size() - done);
            if (n > 0)
                done += static_cast<std::size_t>(n);
            else if (n < 0 && errno == EINTR)
                continue;
            else
                ok = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (spare_.size() < QueueBlocks)
                spare_.push_back(std::move(buffer));
        }
        // helper が終わった (パイプが切れた) 場合。原因は読み取りのスレッドが EOF として扱う
        if (!ok)
            break;
    }
    // 標準入力の終わりが helper への停止要求になる
    CloseInput();
}

void PosixIqProcess::Read() noexcept
{
    try {
        std::array<std::uint8_t, 65536> bytes{};
        for (;;) {
            const ssize_t got = read(output_, bytes.data(), bytes.size());
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            if (!output_state_.Feed(bytes.data(), static_cast<std::size_t>(got), siano::MonotonicMs(), !stopping_,
                                    ts_)) {
                Fail("invalid or failed helper frame");
                return;
            }
        }
    } catch (const std::exception &e) {
        msg_err("PX-S1UD: J.83B helper reader failed: %s\n", e.what());
    }
    if (!stopping_)
        Fail("helper stopped unexpectedly");
}

void PosixIqProcess::Fail(const char *reason)
{
    if (stopping_ || failed_.exchange(true))
        return;
    msg_err("PX-S1UD: J.83B %s\n", reason);
    output_state_.ClearQuality();
    // 書き込み中の write を終わらせるため、helper を止める
    if (pid_ > 0)
        kill(-pid_, SIGKILL);
}

void PosixIqProcess::Stop()
{
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        queue_.clear();
    }
    queued_.notify_all();
    // 書き込みのスレッドが標準入力を閉じると helper は終わる。helper が応答しない場合は
    // 書き込み中の write も戻らないため、期限を過ぎたらプロセスグループごと止めてパイプを切る
    if (pid_ > 0) {
        const auto deadline = std::chrono::steady_clock::now() + StopDeadline;
        int status = 0;
        pid_t done = 0;
        while ((done = waitpid(pid_, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline)
            usleep(10000);
        if (done == 0) {
            msg_warn("PX-S1UD: J.83B helper stop deadline expired; killing process group %d\n", static_cast<int>(pid_));
            kill(-pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        } else if (done > 0 && WIFSIGNALED(status) && !failed_) {
            msg_warn("PX-S1UD: J.83B helper ended by signal %d\n", WTERMSIG(status));
        }
        pid_ = -1;
    }
    if (writer_.joinable())
        writer_.join();
    if (reader_.joinable())
        reader_.join();
    CloseInput();
    CloseFd(output_);
    running_ = false;
    output_state_.Clear();
}
}
