// SPDX-License-Identifier: GPL-2.0-only
#include "rtlsdr_helper.hpp"
#include "msg.h"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace px4::cuse {
namespace {
// 標準入力のパイプの容量。一般のユーザーの上限 (/proc/sys/fs/pipe-max-size、既定 1 MiB) を超える指定は失敗するため、
// 失敗しても既定のまま続ける (iq_process.cpp と同じ)
constexpr int InputPipeSize = 16 << 20;

void CloseFd(int &fd)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
}
}

RtlSdrHelperProcess::~RtlSdrHelperProcess()
{
    if (pid_ > 0)
        Kill();
    Release();
}

int RtlSdrHelperProcess::Spawn(const std::vector<std::string> &argv)
{
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) {
        const int e = errno;
        for (int fd : {in[0], in[1], out[0], out[1]})
            if (fd >= 0)
                close(fd);
        msg_err("RTL-SDR: helper pipe failed: %s\n", std::strerror(e));
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
    std::vector<char *> args;
    for (const auto &arg : argv)
        args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv[0].c_str(), &actions, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    // 子への書込端を親が残すと、子の終了を EOF として検出できなくなる
    close(in[0]);
    close(out[1]);
    if (rc) {
        close(in[1]);
        close(out[0]);
        msg_err("RTL-SDR: helper start failed executable=%s: %s\n", argv[0].c_str(), std::strerror(rc));
        return -rc;
    }
    pid_ = pid;
    output_ = out[0];
    exit_code_ = 259;
    overflow_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_ = in[1];
        closing_ = false;
        queue_.clear();
    }
    try {
        writer_ = std::thread([this] { Write(); });
    } catch (...) {
        Kill();
        return -ENOMEM;
    }
    return 0;
}

bool RtlSdrHelperProcess::Feed(const std::uint8_t *data, std::size_t size)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (closing_ || input_ < 0)
        return true;
    if (queue_.size() >= QueueBlocks) {
        if (!overflow_.exchange(true))
            msg_err("RTL-SDR: helper input queue is full blocks=%zu; restarting the helper\n", queue_.size());
        return false;
    }
    // ブロックごとの確保を避けるため、書き終えたバッファを使い回す
    std::vector<std::uint8_t> buffer;
    if (!spare_.empty()) {
        buffer = std::move(spare_.back());
        spare_.pop_back();
    }
    buffer.assign(data, data + size);
    queue_.push_back(std::move(buffer));
    lock.unlock();
    queued_.notify_one();
    return true;
}

void RtlSdrHelperProcess::Write() noexcept
{
    for (;;) {
        std::vector<std::uint8_t> buffer;
        int fd = -1;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queued_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            // 停止要求では残りを送らずに標準入力を閉じる (選局の切り替えを待たせない)
            if (closing_)
                break;
            buffer = std::move(queue_.front());
            queue_.pop_front();
            fd = input_;
        }
        bool ok = true;
        for (std::size_t done = 0; ok && done < buffer.size();) {
            const ssize_t n = write(fd, buffer.data() + done, buffer.size() - done);
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
        // helper が終わった (パイプが切れた) 場合。原因は Read が EOF として扱う
        if (!ok)
            break;
    }
    // 標準入力の終わりが helper への停止要求になる
    std::lock_guard<std::mutex> lock(mutex_);
    CloseFd(input_);
}

int RtlSdrHelperProcess::Read(std::uint8_t *buffer, std::size_t size, std::chrono::milliseconds wait)
{
    if (overflow_)
        return -ENOBUFS;
    if (output_ < 0)
        return -EPIPE;
    pollfd fd{output_, POLLIN, 0};
    const int ready = poll(&fd, 1, static_cast<int>(wait.count()));
    if (ready < 0)
        return errno == EINTR ? 0 : -errno;
    if (!ready)
        return 0;
    const ssize_t got = read(output_, buffer, size);
    if (got < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : -errno;
    return got ? static_cast<int>(got) : -EPIPE;
}

void RtlSdrHelperProcess::StopWriter()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        queue_.clear();
    }
    queued_.notify_all();
}

void RtlSdrHelperProcess::RequestStop()
{
    StopWriter();
}

void RtlSdrHelperProcess::Reap(int status)
{
    if (WIFEXITED(status))
        exit_code_ = static_cast<std::uint32_t>(WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        exit_code_ = 128 + static_cast<std::uint32_t>(WTERMSIG(status));
}

void RtlSdrHelperProcess::WaitStopped(std::chrono::milliseconds limit)
{
    if (pid_ > 0) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        int status = 0;
        pid_t done = 0;
        while ((done = waitpid(pid_, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline)
            usleep(10000);
        if (done == 0) {
            msg_warn("RTL-SDR: helper stop deadline expired; killing process group %d\n", static_cast<int>(pid_));
            kill(-pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
        Reap(status);
        pid_ = -1;
    }
    // 書き込み中の write は helper の終了でパイプが切れて戻る
    if (writer_.joinable())
        writer_.join();
}

std::uint32_t RtlSdrHelperProcess::Kill()
{
    StopWriter();
    if (pid_ > 0) {
        // 形式不正や応答なしでは helper がまだ動いているため、プロセスグループごと止めてから回収する
        kill(-pid_, SIGKILL);
        int status = 0;
        if (waitpid(pid_, &status, 0) == pid_)
            Reap(status);
        pid_ = -1;
    }
    if (writer_.joinable())
        writer_.join();
    Release();
    return exit_code_;
}

void RtlSdrHelperProcess::Release()
{
    CloseFd(output_);
    std::lock_guard<std::mutex> lock(mutex_);
    CloseFd(input_);
    queue_.clear();
}
}
