// SPDX-License-Identifier: GPL-2.0-only
#include "cuse_session.hpp"
#include "msg.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace px4::cuse {
namespace {
// Stop() が pthread_kill で送り、/dev/cuse の read で待つループのスレッドを EINTR で戻す。
// SA_RESTART を付けると read が再開して戻らない。
void Wake(int) {}
}

void CuseSession::InstallWakeHandler()
{
    struct sigaction action = {};
    action.sa_handler = Wake;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, nullptr);
}

std::unique_ptr<CuseSession> CuseSession::Start(const std::string &devname, const cuse_lowlevel_ops &ops,
                                                unsigned flags, void *userdata, int &error)
{
    std::unique_ptr<CuseSession> self(new CuseSession());
    self->devname_ = devname;
    self->info_ = "DEVNAME=" + devname;
    const char *info_argv[] = {self->info_.c_str()};
    struct cuse_info ci = {};
    ci.dev_info_argc = 1;
    ci.dev_info_argv = info_argv;
    ci.flags = flags;
    char arg0[] = "px4-cused";
    char *argv[] = {arg0, nullptr};
    struct fuse_args args = FUSE_ARGS_INIT(1, argv);
    self->session_ = cuse_lowlevel_new(&args, &ci, &ops, userdata);
    if (!self->session_) {
        error = -ENOMEM;
        return nullptr;
    }
    // cuse_lowlevel_setup と同じく /dev/cuse を開き、その fd をマウント先として渡す。
    const int fd = open("/dev/cuse", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = -errno;
        fuse_session_destroy(self->session_);
        self->session_ = nullptr;
        return nullptr;
    }
    char mountpoint[32];
    std::snprintf(mountpoint, sizeof(mountpoint), "/dev/fd/%d", fd);
    if (fuse_session_mount(self->session_, mountpoint)) {
        close(fd);
        fuse_session_destroy(self->session_);
        self->session_ = nullptr;
        error = -EIO;
        return nullptr;
    }
    auto *raw = self.get();
    try {
        self->thread_ = std::thread([raw] {
            fuse_session_loop(raw->session_);
            raw->done_ = true;
        });
    } catch (...) {
        fuse_session_unmount(self->session_);
        fuse_session_destroy(self->session_);
        self->session_ = nullptr;
        error = -ENOMEM;
        return nullptr;
    }
    error = 0;
    return self;
}

void CuseSession::Stop()
{
    if (stopped_ || !session_)
        return;
    stopped_ = true;
    fuse_session_exit(session_);
    // 別のスレッドで /dev/cuse を閉じても、すでに read で待っているスレッドは起きない。
    // シグナルで read を中断し、ループが終了の記録を確認するまで送り直す (read に入る直前に届いた場合に備える)。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done_ && std::chrono::steady_clock::now() < deadline) {
        pthread_kill(thread_.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done_) {
        // 終了できないループの資源は解放せず、プロセスの終了に任せる。
        msg_err("CUSE session %s did not stop\n", devname_.c_str());
        thread_.detach();
        session_ = nullptr;
        return;
    }
    thread_.join();
    fuse_session_unmount(session_);
    fuse_session_destroy(session_);
    session_ = nullptr;
}
}
