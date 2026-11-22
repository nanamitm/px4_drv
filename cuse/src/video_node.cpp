// SPDX-License-Identifier: GPL-2.0-only
#include "video_node.hpp"
#include "msg.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>

namespace px4::cuse {
namespace {
// read() へ渡すリングの容量。WinUSB 版の受信機 (stream_buf_->Alloc) と同じ 188 × 65536 バイト。
constexpr std::size_t RingSize = 188u * 65536u;
// C/N の読み出しは受信と同じ制御ロックを使う。WinUSB 版と同じく 1 秒に 1 回までとし、間の要求には前回の値を返す。
constexpr auto CnMinInterval = std::chrono::milliseconds(1000);

VideoNode &Self(fuse_req_t req) { return *static_cast<VideoNode *>(fuse_req_userdata(req)); }

void ReplyResult(fuse_req_t req, int rc)
{
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_ioctl(req, 0, nullptr, 0);
}
}

const cuse_lowlevel_ops VideoNode::ops_ = [] {
    cuse_lowlevel_ops ops{};
    ops.open = OnOpen;
    ops.read = OnRead;
    ops.release = OnRelease;
    ops.ioctl = OnIoctl;
    ops.poll = OnPoll;
    return ops;
}();

VideoNode::VideoNode(VideoBackend &backend, const std::atomic_bool &available, std::string label,
                     std::function<void()> changed)
    : backend_(backend), available_(available), label_(std::move(label)), changed_(std::move(changed))
{
}

int VideoNode::Init()
{
    try {
        ring_.resize(RingSize);
    } catch (...) {
        return -ENOMEM;
    }
    return 0;
}

int VideoNode::Start(const std::string &devname)
{
    int error = 0;
    // PTX_ENABLE_LNB_POWER と PTX_SET_SYSTEM_MODE は値を arg で渡すため、制限なしの ioctl で受け取る。
    session_ = CuseSession::Start(devname, ops_, CUSE_UNRESTRICTED_IOCTL, this, error);
    if (session_)
        msg_info("%s: serving /dev/%s\n", label_.c_str(), devname.c_str());
    return error;
}

void VideoNode::Stop()
{
    if (session_)
        session_->Stop();
    session_.reset();
}

bool VideoNode::InUse()
{
    std::lock_guard<std::mutex> state(state_mutex_);
    return open_;
}

void VideoNode::Remove(const std::function<void()> &locked)
{
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        if (streaming_) {
            StopStreamLocked();
            streaming_ = false;
        }
        tuned_ = false;
        locked();
    }
    std::vector<PendingRead> replies;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        EndRingLocked(-EIO, replies);
    }
    ReplyReads(replies, -EIO);
}

void VideoNode::Shutdown(const std::function<void()> &locked)
{
    std::lock_guard<std::mutex> state(state_mutex_);
    if (streaming_) {
        StopStreamLocked();
        streaming_ = false;
    }
    locked();
}

void VideoNode::OnOpen(fuse_req_t req, struct fuse_file_info *fi)
{
    auto &self = Self(req);
    std::lock_guard<std::mutex> state(self.state_mutex_);
    // カーネルの ptx_chrdev_open() と同じく、2 つ目の open は EALREADY にする。
    if (self.open_) {
        fuse_reply_err(req, EALREADY);
        return;
    }
    if (!self.available_) {
        fuse_reply_err(req, ENODEV);
        return;
    }
    if (const int rc = self.backend_.VideoOpen()) {
        fuse_reply_err(req, -rc);
        return;
    }
    self.open_ = true;
    self.failed_ = self.tuned_ = false;
    self.cn_cached_ = false;
    self.system_ = PTX_UNSPECIFIED_SYSTEM;
    // カーネルの ptx_chrdev_release() と同じく、J.83B などの拡張 ioctl の方式を次の利用者へ持ち越さない
    self.params_system_ = self.params_khz_ = 0;
    {
        std::lock_guard<std::mutex> ring(self.ring_mutex_);
        self.head_ = self.fill_ = 0;
        self.live_ = false;
        self.ring_error_ = 0;
    }
    fi->nonseekable = 1;
    fi->direct_io = 1;
    fuse_reply_open(req, fi);
}

void VideoNode::OnRelease(fuse_req_t req, struct fuse_file_info *)
{
    auto &self = Self(req);
    {
        std::lock_guard<std::mutex> state(self.state_mutex_);
        if (self.streaming_) {
            self.StopStreamLocked();
            self.streaming_ = false;
        }
        self.backend_.VideoClose();
        self.tuned_ = false;
        std::lock_guard<std::mutex> ring(self.ring_mutex_);
        self.head_ = self.fill_ = 0;
        self.live_ = false;
        if (self.poll_) {
            fuse_pollhandle_destroy(self.poll_);
            self.poll_ = nullptr;
        }
        self.open_ = false;
    }
    fuse_reply_err(req, 0);
    if (!self.available_ && self.changed_)
        self.changed_();
}

void VideoNode::ReadInterrupted(fuse_req_t req, void *data)
{
    auto &self = *static_cast<VideoNode *>(data);
    bool found = false;
    {
        std::lock_guard<std::mutex> ring(self.ring_mutex_);
        const auto it = std::find_if(self.reads_.begin(), self.reads_.end(),
                                     [req](const PendingRead &read) { return read.req == req; });
        if (it != self.reads_.end()) {
            self.reads_.erase(it);
            found = true;
        }
    }
    if (found)
        fuse_reply_err(req, EINTR);
}

void VideoNode::OnRead(fuse_req_t req, size_t size, off_t, struct fuse_file_info *fi)
{
    auto &self = Self(req);
    // リングは 188 バイト単位で保つため、1 パケットに満たない読み出しは受け付けない。
    if (size < 188) {
        fuse_reply_err(req, EINVAL);
        return;
    }
    bool registered = false;
    for (;;) {
        std::vector<std::uint8_t> out;
        int error = 0;
        bool eof = false;
        {
            std::lock_guard<std::mutex> ring(self.ring_mutex_);
            if (self.fill_ >= 188) {
                const std::size_t n = std::min(size / 188 * 188, self.fill_ / 188 * 188);
                out.resize(n);
                const std::size_t first = std::min(n, RingSize - self.head_);
                std::memcpy(out.data(), &self.ring_[self.head_], first);
                std::memcpy(out.data() + first, &self.ring_[0], n - first);
                self.head_ = (self.head_ + n) % RingSize;
                self.fill_ -= n;
            } else if (!self.live_) {
                // 停止後に読み尽くしたら EOF。切断や受信の異常の後はその値で失敗させる。
                error = self.ring_error_;
                eof = !error;
            } else if (fi->flags & O_NONBLOCK) {
                error = -EAGAIN;
            } else if (registered) {
                if (fuse_req_interrupted(req)) {
                    error = -EINTR;
                } else {
                    // CUSE のループを止めないよう、要求を保留して TS が届いたときに応答する。
                    self.reads_.push_back({req, size});
                    return;
                }
            }
        }
        if (!out.empty()) {
            fuse_reply_buf(req, reinterpret_cast<const char *>(out.data()), out.size());
            return;
        }
        if (eof) {
            fuse_reply_buf(req, nullptr, 0);
            return;
        }
        if (error) {
            fuse_reply_err(req, -error);
            return;
        }
        // 割り込みの処理は、保留して他のスレッドが応答できるようにする前に登録する。
        // 登録の時点ですでに割り込まれていた場合は ReadInterrupted がその場で呼ばれ (保留前なので何もしない)、
        // 次の周回の fuse_req_interrupted() で EINTR を返す。
        fuse_req_interrupt_func(req, ReadInterrupted, &self);
        registered = true;
    }
}

void VideoNode::OnPoll(fuse_req_t req, struct fuse_file_info *, struct fuse_pollhandle *ph)
{
    auto &self = Self(req);
    unsigned mask = 0;
    std::lock_guard<std::mutex> ring(self.ring_mutex_);
    if (self.fill_ >= 188 || !self.live_)
        mask |= POLLIN | POLLRDNORM;
    if (ph) {
        if (self.poll_)
            fuse_pollhandle_destroy(self.poll_);
        self.poll_ = ph;
    }
    fuse_reply_poll(req, mask);
}

void VideoNode::OnIoctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *, unsigned flags, const void *in,
                        size_t in_size, size_t out_size)
{
    auto &self = Self(req);
    const auto ucmd = static_cast<unsigned>(cmd);
    const auto value = reinterpret_cast<std::uintptr_t>(arg);
    // 32 ビットのプロセスへの対応は未決のため、カーネルモジュールと同じく compat の要求は受け付けない。
    if (flags & FUSE_IOCTL_COMPAT) {
        fuse_reply_err(req, ENOSYS);
        return;
    }
    if (!self.available_) {
        fuse_reply_err(req, EIO);
        return;
    }
    switch (ucmd) {
    case PTX_SET_CHANNEL: {
        if (in_size < sizeof(ptx_freq)) {
            struct iovec iov = {arg, sizeof(ptx_freq)};
            fuse_reply_ioctl_retry(req, &iov, 1, nullptr, 0);
            return;
        }
        ptx_freq freq;
        std::memcpy(&freq, in, sizeof(freq));
        ReplyResult(req, self.SetChannel(freq));
        return;
    }
    case PTX_START_STREAMING: {
        std::lock_guard<std::mutex> state(self.state_mutex_);
        int rc = self.failed_ ? -EIO : self.streaming_ ? -EALREADY : !self.tuned_ ? -EINVAL : self.StartStreamLocked();
        if (!rc)
            self.streaming_ = true;
        msg_info("%s: start streaming result=%d\n", self.label_.c_str(), rc);
        ReplyResult(req, rc);
        return;
    }
    case PTX_STOP_STREAMING: {
        std::lock_guard<std::mutex> state(self.state_mutex_);
        if (!self.streaming_) {
            fuse_reply_err(req, EALREADY);
            return;
        }
        self.StopStreamLocked();
        self.streaming_ = false;
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    }
    case PTX_GET_CNR: {
        if (out_size < sizeof(std::uint32_t)) {
            // 番号が示す大きさ (ポインタの 8 バイト) ではなく、カーネルと同じ int の 4 バイトだけを書き戻す。
            struct iovec iov = {arg, sizeof(std::uint32_t)};
            fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
            return;
        }
        std::uint32_t raw = 0;
        const int rc = self.ReadCn(raw);
        if (rc)
            fuse_reply_err(req, -rc);
        else
            fuse_reply_ioctl(req, 0, &raw, sizeof(raw));
        return;
    }
    case PTX_GET_LOCK_STATUS: {
        if (out_size < sizeof(std::uint32_t)) {
            struct iovec iov = {arg, sizeof(std::uint32_t)};
            fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
            return;
        }
        std::lock_guard<std::mutex> state(self.state_mutex_);
        if (!self.tuned_) {
            fuse_reply_err(req, EAGAIN);
            return;
        }
        bool locked = false;
        const int rc = self.backend_.LockStatus(locked);
        std::uint32_t status = locked ? 1 : 0;
        if (rc)
            fuse_reply_err(req, -rc);
        else
            fuse_reply_ioctl(req, 0, &status, sizeof(status));
        return;
    }
    case PTX_ENABLE_LNB_POWER:
        // アンテナ給電を持たないため、カーネルの set_lnb_voltage を持たない機種と同じ応答にする。
        ReplyResult(req, value ? -ENOSYS : 0);
        return;
    case PTX_DISABLE_LNB_POWER:
        fuse_reply_ioctl(req, 0, nullptr, 0);
        return;
    case PTX_SET_SYSTEM_MODE: {
        std::lock_guard<std::mutex> state(self.state_mutex_);
        if (value <= UINT32_MAX && self.backend_.AcceptsSystem(static_cast<std::uint32_t>(value))) {
            self.system_ = static_cast<std::uint32_t>(value);
            fuse_reply_ioctl(req, 0, nullptr, 0);
        } else {
            fuse_reply_err(req, EINVAL);
        }
        return;
    }
    case PTXT_SET_PARAMS: {
        if (in_size < sizeof(ptxt_params)) {
            struct iovec iov = {arg, sizeof(ptxt_params)};
            fuse_reply_ioctl_retry(req, &iov, 1, nullptr, 0);
            return;
        }
        ptxt_params params;
        std::memcpy(&params, in, sizeof(params));
        ReplyResult(req, self.SetParams(params));
        return;
    }
    case PTXT_TUNE:
        ReplyResult(req, self.TuneParams());
        return;
    default:
        fuse_reply_err(req, ENOSYS);
        return;
    }
}

int VideoNode::SetParams(const ptxt_params &params)
{
    std::lock_guard<std::mutex> state(state_mutex_);
    // カーネルの ptx_chrdev_set_params() と同じ検査。追加パラメーターは ISDB-C の TSMF の TS 番号だけで、
    // CUSE の機種は ISDB-C を扱わないため、配列を読まずに 0 個以外を断る
    if (!params.system || !backend_.AcceptsParams(params.system) || !params.freq || params.reserved ||
        params.num_prop)
        return -EINVAL;
    params_system_ = params.system;
    params_khz_ = params.freq;
    return 0;
}

int VideoNode::TuneParams()
{
    std::lock_guard<std::mutex> state(state_mutex_);
    if (failed_)
        return -EIO;
    if (!params_system_)
        return -EINVAL;
    tuned_ = false;
    cn_cached_ = false;
    bool fatal = false;
    const int rc = backend_.TuneParams(params_system_, params_khz_, streaming_, fatal);
    if (fatal)
        failed_ = true;
    tuned_ = rc == 0;
    return rc;
}

int VideoNode::SetChannel(const ptx_freq &freq)
{
    std::lock_guard<std::mutex> state(state_mutex_);
    if (failed_)
        return -EIO;
    tuned_ = false;
    cn_cached_ = false;
    bool fatal = false;
    const int rc = backend_.SetChannel(system_, freq, streaming_, fatal);
    if (fatal)
        failed_ = true;
    tuned_ = rc == 0;
    return rc;
}

int VideoNode::ReadCn(std::uint32_t &raw)
{
    std::lock_guard<std::mutex> state(state_mutex_);
    if (failed_)
        return -EIO;
    if (!tuned_)
        return -EAGAIN;
    if (!cn_cached_ || Clock::now() - cn_read_at_ >= CnMinInterval) {
        std::uint32_t value = 0;
        const int rc = backend_.ReadCn(value);
        if (rc)
            return rc;
        cn_raw_ = value;
        cn_read_at_ = Clock::now();
        cn_cached_ = true;
    }
    raw = cn_raw_;
    return 0;
}

int VideoNode::StartStreamLocked()
{
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        head_ = fill_ = 0;
        overflow_ = 0;
        ring_error_ = 0;
        live_ = true;
    }
    bool fatal = false;
    const int rc = backend_.StartStream(fatal);
    if (fatal)
        failed_ = true;
    if (rc) {
        std::vector<PendingRead> replies;
        {
            std::lock_guard<std::mutex> ring(ring_mutex_);
            EndRingLocked(0, replies);
        }
        ReplyReads(replies, 0);
    }
    return rc;
}

void VideoNode::StopStreamLocked()
{
    bool fatal = false;
    const int error = backend_.StopStream(fatal);
    if (fatal)
        failed_ = true;
    std::vector<PendingRead> replies;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        // 残りの TS は read() で読み出せ、読み尽くすと EOF になる。
        EndRingLocked(error, replies);
    }
    ReplyReads(replies, error);
}

void VideoNode::Push(const std::uint8_t *data, std::size_t size)
{
    std::vector<std::pair<PendingRead, std::vector<std::uint8_t>>> replies;
    struct fuse_pollhandle *poll = nullptr;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        if (!live_)
            return;
        if (fill_ + size > RingSize) {
            // 利用者の読み出しが追いつかないときは、WinUSB 版と同じく新しいパケットを捨てて受信を続ける。
            overflow_ += size;
            return;
        }
        const std::size_t tail = (head_ + fill_) % RingSize;
        const std::size_t first = std::min(size, RingSize - tail);
        std::memcpy(&ring_[tail], data, first);
        std::memcpy(&ring_[0], data + first, size - first);
        fill_ += size;
        while (!reads_.empty() && fill_ >= 188) {
            const auto read = reads_.front();
            reads_.pop_front();
            const std::size_t n = std::min(read.size / 188 * 188, fill_ / 188 * 188);
            std::vector<std::uint8_t> out(n);
            const std::size_t part = std::min(n, RingSize - head_);
            std::memcpy(out.data(), &ring_[head_], part);
            std::memcpy(out.data() + part, &ring_[0], n - part);
            head_ = (head_ + n) % RingSize;
            fill_ -= n;
            replies.emplace_back(read, std::move(out));
        }
        poll = poll_;
        poll_ = nullptr;
    }
    for (auto &reply : replies)
        fuse_reply_buf(reply.first.req, reinterpret_cast<const char *>(reply.second.data()), reply.second.size());
    if (poll) {
        fuse_lowlevel_notify_poll(poll);
        fuse_pollhandle_destroy(poll);
    }
}

void VideoNode::Fail(int error)
{
    std::vector<PendingRead> replies;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        EndRingLocked(error, replies);
    }
    ReplyReads(replies, error);
}

std::uint64_t VideoNode::Overflow()
{
    std::lock_guard<std::mutex> ring(ring_mutex_);
    return overflow_;
}

void VideoNode::EndRingLocked(int error, std::vector<PendingRead> &replies)
{
    live_ = false;
    if (error && !ring_error_)
        ring_error_ = error;
    replies.assign(reads_.begin(), reads_.end());
    reads_.clear();
    if (poll_) {
        fuse_lowlevel_notify_poll(poll_);
        fuse_pollhandle_destroy(poll_);
        poll_ = nullptr;
    }
}

void VideoNode::ReplyReads(std::vector<PendingRead> &replies, int error)
{
    for (const auto &read : replies) {
        if (error)
            fuse_reply_err(read.req, -error);
        else
            fuse_reply_buf(read.req, nullptr, 0);
    }
}
}
