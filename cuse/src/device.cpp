// SPDX-License-Identifier: GPL-2.0-only
#include "device.hpp"
#include "channel.hpp"
#include "msg.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>

namespace px4::cuse {
namespace {
// read() へ渡すリングの容量。WinUSB 版の受信機 (stream_buf_->Alloc) と同じ 188 × 65536 バイト。
constexpr std::size_t RingSize = 188u * 65536u;
// C/N の読み出しは SERVICE と同じ制御ロックを使う。WinUSB 版と同じく 1 秒に 1 回までとし、間の要求には前回の値を返す。
constexpr auto CnMinInterval = std::chrono::milliseconds(1000);
// Bulk IN の読み出し。WinUSB 版の direct-fr と同じ 8 KiB × 64 本。
constexpr std::size_t BulkCount = 64, BulkSize = 8192;

std::uint64_t Micros(Device::Clock::duration d)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

Device &Self(fuse_req_t req) { return *static_cast<Device *>(fuse_req_userdata(req)); }

void ReplyResult(fuse_req_t req, int rc)
{
    if (rc)
        fuse_reply_err(req, -rc);
    else
        fuse_reply_ioctl(req, 0, nullptr, 0);
}

// カーネルの px4_card_reader_id() と WinUSB 版の HashDevicePath() と同じ ID を求める。
// GUID は winusb/pkg/inf/PIX-DT295.inf の DeviceInterfaceGUIDs。
bool CardReaderId(libusb_device_handle *handle, std::string &id)
{
    libusb_device_descriptor desc{};
    unsigned char serial[256] = {};
    if (libusb_get_device_descriptor(libusb_get_device(handle), &desc) || !desc.iSerialNumber ||
        libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, serial, sizeof(serial)) <= 0)
        return false;
    const std::string s(reinterpret_cast<const char *>(serial));
    if (s.empty() || s == "." || s == ".." ||
        s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos)
        return false;
    char path[512];
    std::snprintf(path, sizeof(path), "\\\\?\\usb#vid_%04x&pid_%04x#%s#%s", desc.idVendor, desc.idProduct, s.c_str(),
                  "{f5b71c91-0634-4a6b-902f-fd5756f591d3}");
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char *p = path; *p; ++p) {
        hash ^= static_cast<std::uint16_t>(std::tolower(static_cast<unsigned char>(*p)));
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llX", static_cast<unsigned long long>(hash));
    id = out;
    return true;
}
}

const cuse_lowlevel_ops Device::video_ops_ = [] {
    cuse_lowlevel_ops ops{};
    ops.open = VideoOpen;
    ops.read = VideoRead;
    ops.release = VideoRelease;
    ops.ioctl = VideoIoctl;
    ops.poll = VideoPoll;
    return ops;
}();

const cuse_lowlevel_ops Device::card_ops_ = [] {
    cuse_lowlevel_ops ops{};
    ops.open = CardOpen;
    ops.release = CardRelease;
    ops.ioctl = CardIoctl;
    return ops;
}();

Device::Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed)
    : handle_(handle), bus_(bus), address_(address), changed_(std::move(changed)), transport_(handle, removal_),
      channel_(source_, transport_, available_, [this](unsigned ms) { return removal_.Wait(ms); })
{
}

Device::~Device()
{
    StopSessions();
}

int Device::Init()
{
    if (const int rc = source_.LoadEmbedded())
        return rc;
    if (!CardReaderId(handle_, card_id_))
        msg_warn("PIX-DT295 %03u:%03u: serial number unavailable; card reader disabled\n", bus_, address_);
    try {
        ring_.resize(RingSize);
    } catch (...) {
        return -ENOMEM;
    }
    return 0;
}

int Device::StartVideo(unsigned index)
{
    int error = 0;
    char name[32];
    std::snprintf(name, sizeof(name), "dt295video%u", index);
    // PTX_ENABLE_LNB_POWER と PTX_SET_SYSTEM_MODE は値を arg で渡すため、制限なしの ioctl で受け取る。
    video_ = CuseSession::Start(name, video_ops_, CUSE_UNRESTRICTED_IOCTL, this, error);
    if (video_) {
        video_index_ = index;
        msg_info("PIX-DT295 %03u:%03u: serving /dev/%s\n", bus_, address_, name);
    }
    return error;
}

int Device::StartCard()
{
    if (card_id_.empty())
        return -ENODEV;
    int error = 0;
    // DEVNAME の '!' は udev が '/' に置き換え、/dev/px4card/<ID> になる。
    card_session_ = CuseSession::Start("px4card!" + card_id_, card_ops_, 0, this, error);
    if (card_session_)
        msg_info("PIX-DT295 %03u:%03u: serving /dev/px4card/%s\n", bus_, address_, card_id_.c_str());
    return error;
}

void Device::StopSessions()
{
    if (card_session_)
        card_session_->Stop();
    if (video_)
        video_->Stop();
    card_session_.reset();
    video_.reset();
}

bool Device::StopUnusedSessions()
{
    bool video_unused = false, card_unused = false;
    {
        std::scoped_lock lock(state_mutex_, card_mutex_);
        video_unused = !open_;
        card_unused = !card_open_;
    }
    // 切断後の open は ENODEV で断るため、判定の後に開かれることはない。
    if (video_unused && video_) {
        video_->Stop();
        video_.reset();
    }
    if (card_unused && card_session_) {
        card_session_->Stop();
        card_session_.reset();
        return true;
    }
    return false;
}

bool Device::Idle()
{
    std::scoped_lock lock(state_mutex_, card_mutex_);
    return !open_ && !card_open_;
}

void Device::Remove()
{
    available_ = false;
    removal_.Set();
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        if (streaming_) {
            StopStream();
            streaming_ = false;
        }
        // 切断後の機器へは復元の転送を行えないため、初期化済みの状態も破棄する。
        initialized_ = tuned_ = false;
    }
    std::vector<PendingRead> replies;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        EndRingLocked(-EIO, replies);
    }
    ReplyReads(replies, -EIO);
    std::lock_guard<std::mutex> card(card_mutex_);
    card_powered_ = false;
    card_.Discard();
}

void Device::Shutdown()
{
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        if (streaming_) {
            StopStream();
            streaming_ = false;
        }
        if (initialized_ && available_)
            RestoreLocked();
    }
    std::lock_guard<std::mutex> card(card_mutex_);
    CardPowerOffLocked();
}

// ---- 受信のデバイスファイル ----

void Device::VideoOpen(fuse_req_t req, struct fuse_file_info *fi)
{
    auto &self = Self(req);
    std::lock_guard<std::mutex> state(self.state_mutex_);
    // カーネルの ptx_chrdev_open() と同じく、2 つ目の open は EALREADY にする。
    if (self.open_) {
        fuse_reply_err(req, EALREADY);
        return;
    }
    if (!self.available_ || self.restore_failed_) {
        fuse_reply_err(req, ENODEV);
        return;
    }
    self.open_ = true;
    self.failed_ = false;
    self.system_ = PTX_UNSPECIFIED_SYSTEM;
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

void Device::VideoRelease(fuse_req_t req, struct fuse_file_info *)
{
    auto &self = Self(req);
    {
        std::lock_guard<std::mutex> state(self.state_mutex_);
        if (self.streaming_) {
            self.StopStream();
            self.streaming_ = false;
        }
        // WinUSB 版の受信機の Close と同じく、閉じるたびに機器を起動前の状態へ戻す。
        if (self.initialized_ && self.available_)
            self.RestoreLocked();
        self.initialized_ = self.tuned_ = false;
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

void Device::ReadInterrupted(fuse_req_t req, void *data)
{
    auto &self = *static_cast<Device *>(data);
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

void Device::VideoRead(fuse_req_t req, size_t size, off_t, struct fuse_file_info *fi)
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
                // 停止後に読み尽くしたら EOF。切断や SERVICE の失敗の後はその値で失敗させる。
                error = self.ring_error_;
                eof = !error;
            } else if (fi->flags & O_NONBLOCK) {
                error = -EAGAIN;
            } else if (registered) {
                if (fuse_req_interrupted(req)) {
                    error = -EINTR;
                } else {
                    // CUSE のループを止めないよう、要求を保留して Bulk IN の完了時に応答する。
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

void Device::VideoPoll(fuse_req_t req, struct fuse_file_info *, struct fuse_pollhandle *ph)
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

void Device::VideoIoctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *, unsigned flags,
                        const void *in, size_t in_size, size_t out_size)
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
        int rc = self.failed_ ? -EIO : self.streaming_ ? -EALREADY : !self.tuned_ ? -EINVAL : self.StartStream();
        if (!rc)
            self.streaming_ = true;
        msg_info("PIX-DT295 %03u:%03u: start streaming result=%d\n", self.bus_, self.address_, rc);
        ReplyResult(req, rc);
        return;
    }
    case PTX_STOP_STREAMING: {
        std::lock_guard<std::mutex> state(self.state_mutex_);
        if (!self.streaming_) {
            fuse_reply_err(req, EALREADY);
            return;
        }
        self.StopStream();
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
        // Locked() は選局で検証した状態を返すだけで USB 転送を行わないため、制御ロックを取らない。
        int locked = 0;
        const int rc = self.source_.Locked(locked);
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
        if (value == PTX_UNSPECIFIED_SYSTEM || value == PTX_ISDB_T_SYSTEM || value == PTX_ISDB_S_SYSTEM) {
            self.system_ = static_cast<std::uint32_t>(value);
            fuse_reply_ioctl(req, 0, nullptr, 0);
        } else {
            fuse_reply_err(req, EINVAL);
        }
        return;
    }
    default:
        fuse_reply_err(req, ENOSYS);
        return;
    }
}

int Device::EnsureInitialized()
{
    if (initialized_)
        return 0;
    auto command = channel_.Lock();
    msg_info("PIX-DT295 %03u:%03u: initializing backend=direct-fr\n", bus_, address_);
    transport_.Begin(120000);
    const int rc = source_.Start(transport_.Callbacks());
    transport_.ReportTiming("initialize");
    if (rc) {
        msg_err("PIX-DT295 %03u:%03u: initialize failed result=%d libusb=%lu\n", bus_, address_, rc,
                transport_.LastError());
        failed_ = true;
        return rc;
    }
    initialized_ = true;
    return 0;
}

void Device::RestoreLocked()
{
    // カードの操作と転送が混ざらないよう、復元も制御ロックの下で行う。
    auto command = channel_.Lock();
    transport_.BeginRestore(120000);
    const int rc = source_.Close();
    transport_.ReportTiming("source-close");
    // 復元できなかった機器は、WinUSB 版と同じく挿し直すまで開かせない。
    restore_failed_ = rc != 0;
    if (rc)
        msg_err("PIX-DT295 %03u:%03u: restore failed result=%d\n", bus_, address_, rc);
    initialized_ = tuned_ = false;
}

int Device::SetChannel(const ptx_freq &freq)
{
    std::lock_guard<std::mutex> state(state_mutex_);
    if (failed_)
        return -EIO;
    Tune tune;
    int rc = ChannelToTune(system_, freq, tune);
    if (!rc && streaming_)
        rc = -EBUSY;
    if (!rc)
        rc = EnsureInitialized();
    if (!rc) {
        tuned_ = false;
        cn_cached_ = false;
        auto command = channel_.Lock();
        transport_.Begin(20000);
        // 衛星の tsid 0〜7 は、共有コードが相対 TS 番号として TMCC から TSID を求める。
        rc = source_.Tune(tune.satellite, tune.khz, tune.tsid);
        transport_.ReportTiming("tune");
        // 引数不正、無信号 (-ENOLINK)、存在しない相対 TS 番号や TSID の不一致 (-EAGAIN) は機器の状態を壊さないため、
        // 次の選局を受け付ける。それ以外は転送の異常として、閉じて復元するまで操作を断る。
        if (rc && rc != -EINVAL && rc != -ENOLINK && rc != -EAGAIN)
            failed_ = true;
    }
    msg_info("PIX-DT295 %03u:%03u: set channel freq_no=%d slot=%d -> %s%s kHz=%u tsid=%u result=%d\n", bus_, address_,
             freq.freq_no, freq.slot, tune.satellite ? "ISDB-S" : "ISDB-T", tune.relative ? " relative" : "", tune.khz,
             tune.tsid, rc);
    tuned_ = rc == 0;
    // 復調器 TC90532XBG は地デジのロック直後に訂正前の TS を数十 ms 出力するため、WinUSB 版
    // (RECEIVER_WAIT_AFTER_LOCK_TC_T) と同じく 350 ms 待ってから選局を完了する。
    // TS の取り込みは START_STREAMING から始まるため、この間の TS は利用者へ渡らない。
    if (tuned_ && !tune.satellite)
        removal_.Wait(350);
    return rc;
}

int Device::ReadCn(std::uint32_t &raw)
{
    std::lock_guard<std::mutex> state(state_mutex_);
    if (failed_)
        return -EIO;
    if (!tuned_)
        return -EAGAIN;
    if (!cn_cached_ || Clock::now() - cn_read_at_ >= CnMinInterval) {
        bool satellite = false;
        std::uint32_t value = 0;
        const int rc = channel_.ReadCn(satellite, value);
        if (rc)
            return rc;
        cn_raw_ = value;
        cn_read_at_ = Clock::now();
        cn_cached_ = true;
    }
    raw = cn_raw_;
    return 0;
}

// ---- TS の受信 ----

void Device::RingPush(const std::uint8_t *data, std::size_t size)
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

void Device::EndRingLocked(int error, std::vector<PendingRead> &replies)
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

void Device::ReplyReads(std::vector<PendingRead> &replies, int error)
{
    for (const auto &read : replies) {
        if (error)
            fuse_reply_err(read.req, -error);
        else
            fuse_reply_buf(read.req, nullptr, 0);
    }
}

void LIBUSB_CALL Device::BulkDone(libusb_transfer *transfer)
{
    auto &self = *static_cast<Device *>(transfer->user_data);
    auto &s = self.stream_;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        const auto now = Clock::now();
        if (s.previous_read != Clock::time_point{})
            s.max_read_gap_us = std::max(s.max_read_gap_us, Micros(now - s.previous_read));
        s.previous_read = now;
        if (s.deliver && transfer->actual_length) {
            const int rc = s.delivery->Deliver(self.source_, transfer->buffer,
                                               static_cast<std::size_t>(transfer->actual_length),
                                               [&self](const std::uint8_t *packets, std::size_t size) {
                                                   self.RingPush(packets, size);
                                               });
            if (rc)
                s.error = rc;
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        // 切断では投入済みの 64 本がまとめて失敗するため、最初の 1 本だけを記録する。
        int expected = 0;
        const int error = transfer->status == LIBUSB_TRANSFER_NO_DEVICE ? -ENODEV : -EIO;
        if (s.error.compare_exchange_strong(expected, error))
            msg_err("PIX-DT295 %03u:%03u: bulk status=%d\n", self.bus_, self.address_, transfer->status);
    }
    if (s.running && !s.error && transfer->status == LIBUSB_TRANSFER_COMPLETED && !libusb_submit_transfer(transfer))
        return;
    bool idle = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        idle = --s.in_flight == 0;
    }
    if (idle)
        s.idle.notify_all();
    // Bulk IN の異常は SERVICE の停止を待たずに利用者の read へ伝える。
    if (s.error) {
        std::vector<PendingRead> replies;
        {
            std::lock_guard<std::mutex> ring(self.ring_mutex_);
            self.EndRingLocked(s.error, replies);
        }
        ReplyReads(replies, s.error);
    }
}

int Device::StartStream()
{
    auto &s = stream_;
    try {
        s.delivery = std::make_unique<dt295::TsDelivery>();
        s.buffers.assign(BulkCount, std::vector<std::uint8_t>(BulkSize));
    } catch (...) {
        return -ENOMEM;
    }
    s.error = 0;
    s.stop = false;
    s.previous_read = Clock::time_point{};
    s.max_read_gap_us = 0;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        head_ = fill_ = 0;
        overflow_ = 0;
        ring_error_ = 0;
        live_ = true;
    }
    s.running = true;
    for (auto &buffer : s.buffers) {
        auto *transfer = libusb_alloc_transfer(0);
        if (!transfer) {
            StopStream();
            return -ENOMEM;
        }
        libusb_fill_bulk_transfer(transfer, handle_, 0x81, buffer.data(), static_cast<int>(buffer.size()), BulkDone,
                                  this, 0);
        s.transfers.push_back(transfer);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.in_flight;
        }
        if (libusb_submit_transfer(transfer)) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                --s.in_flight;
            }
            StopStream();
            return -EIO;
        }
    }
    int rc;
    {
        auto command = channel_.Lock();
        transport_.Begin(20000);
        rc = source_.Capture(true);
    }
    if (rc) {
        StopStream();
        failed_ = true;
        return rc;
    }
    // input の初期化が終わる前の USB のデータは破棄し、フレーマーとの競合を避ける。
    s.deliver = true;
    try {
        s.service = std::thread([this] {
            auto &s = stream_;
            dt295::ControlChannel::ServiceStats stats;
            std::time_t max_at = 0;
            std::uint64_t max_us = 0;
            const int rc = channel_.RunService(
                [this] { return !available_ || stream_.stop || stream_.error; },
                [&max_at, &max_us](Clock::time_point begin, Clock::time_point completed, int) {
                    // SERVICE の所要時間の最大値が、受信中と停止の手順のどちらで起きたかを区別できるよう時刻を残す。
                    const auto us = Micros(completed - begin);
                    if (us > max_us) {
                        max_us = us;
                        max_at = std::time(nullptr);
                    }
                },
                stats);
            if (rc) {
                msg_err("PIX-DT295 %03u:%03u: SERVICE failed result=%d\n", bus_, address_, rc);
                s.error = rc;
                std::vector<PendingRead> replies;
                {
                    std::lock_guard<std::mutex> ring(ring_mutex_);
                    EndRingLocked(rc, replies);
                }
                ReplyReads(replies, rc);
            }
            char at[16] = "-";
            std::tm local{};
            if (max_at && localtime_r(&max_at, &local))
                std::strftime(at, sizeof(at), "%H:%M:%S", &local);
            msg_info("PIX-DT295 %03u:%03u: service calls=%llu mean_gap_us=%llu max_gap_us=%llu max_call_us=%llu "
                     "max_call_at=%s\n",
                     bus_, address_, static_cast<unsigned long long>(stats.calls),
                     static_cast<unsigned long long>(stats.calls ? stats.total_gap_us / stats.calls : 0),
                     static_cast<unsigned long long>(stats.max_gap_us), static_cast<unsigned long long>(stats.max_call_us),
                     at);
        });
    } catch (...) {
        StopStream();
        return -ENOMEM;
    }
    return 0;
}

void Device::StopStream()
{
    auto &s = stream_;
    s.stop = true;
    if (s.service.joinable())
        s.service.join();
    s.running = false;
    s.deliver = false;
    for (auto *transfer : s.transfers)
        libusb_cancel_transfer(transfer);
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.idle.wait_for(lock, std::chrono::seconds(5), [&s] { return s.in_flight == 0; }))
            msg_err("PIX-DT295 %03u:%03u: %d bulk transfers did not complete\n", bus_, address_, s.in_flight);
    }
    const bool all_done = [&s] {
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.in_flight == 0;
    }();
    // 完了しなかった転送は libusb が使い続ける可能性があるため、解放せずに残す。
    if (all_done) {
        for (auto *transfer : s.transfers)
            libusb_free_transfer(transfer);
    }
    s.transfers.clear();
    if (available_) {
        auto command = channel_.Lock();
        transport_.Begin(20000);
        const int rc = source_.Capture(false);
        transport_.ReportTiming("capture-stop");
        if (rc) {
            msg_err("PIX-DT295 %03u:%03u: capture stop result=%d\n", bus_, address_, rc);
            failed_ = true;
        }
    }
    std::uint64_t discarded = 0, desyncs = 0;
    source_.InputStats(discarded, desyncs);
    px4::TsContinuityMonitor::Counters q{};
    if (s.delivery)
        q = s.delivery->Counts();
    std::uint64_t overflow = 0;
    std::vector<PendingRead> replies;
    {
        std::lock_guard<std::mutex> ring(ring_mutex_);
        overflow = overflow_;
        // 残りの TS は read() で読み出せ、読み尽くすと EOF になる。
        EndRingLocked(s.error, replies);
    }
    ReplyReads(replies, s.error);
    msg_info("PIX-DT295 %03u:%03u: stream end packets=%llu framing_desync=%llu sync=%llu tei=%llu cc_jump=%llu "
             "malformed=%llu ring_overflow=%llu max_read_gap_us=%llu error=%d\n",
             bus_, address_, static_cast<unsigned long long>(q.packets), static_cast<unsigned long long>(desyncs),
             static_cast<unsigned long long>(q.sync), static_cast<unsigned long long>(q.tei),
             static_cast<unsigned long long>(q.cc_jump), static_cast<unsigned long long>(q.malformed),
             static_cast<unsigned long long>(overflow), static_cast<unsigned long long>(s.max_read_gap_us),
             s.error.load());
    s.delivery.reset();
}

// ---- カードのデバイスファイル ----

void Device::CardOpen(fuse_req_t req, struct fuse_file_info *fi)
{
    auto &self = Self(req);
    std::lock_guard<std::mutex> card(self.card_mutex_);
    if (!self.available_) {
        fuse_reply_err(req, ENODEV);
        return;
    }
    // カーネルと同じく、同時に開けるのは 1 つだけ。
    if (self.card_open_) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    self.card_open_ = true;
    fi->nonseekable = 1;
    fuse_reply_open(req, fi);
}

void Device::CardRelease(fuse_req_t req, struct fuse_file_info *)
{
    auto &self = Self(req);
    {
        std::lock_guard<std::mutex> card(self.card_mutex_);
        self.CardPowerOffLocked();
        self.card_open_ = false;
    }
    fuse_reply_err(req, 0);
    if (!self.available_ && self.changed_)
        self.changed_();
}

// card_mutex_ を保持して呼ぶ。失敗しても電源を切った状態として扱う (カーネルの POWER_OFF と同じ)。
void Device::CardPowerOffLocked()
{
    if (!card_powered_)
        return;
    card_powered_ = false;
    card_.Discard();
    if (!available_)
        return;
    if (const int rc = card_.PowerDown(channel_))
        msg_warn("PIX-DT295 %03u:%03u: card power down failed result=%d\n", bus_, address_, rc);
}

int Device::CardRequest(unsigned cmd, std::uint32_t &value, px4_card_frame &frame)
{
    std::lock_guard<std::mutex> card(card_mutex_);
    // 切断後のコマンドは ENODEV を返す (include/px4_card_ioctl.h)。
    if (!available_)
        return -ENODEV;
    // カーネルの px4_card_request() と同じく、電源投入前の UART の操作は EHOSTDOWN にする。
    if (cmd != PX4_CARD_POWER_ON && cmd != PX4_CARD_POWER_OFF && cmd != PX4_CARD_DETECT && !card_powered_)
        return -EHOSTDOWN;
    switch (cmd) {
    case PX4_CARD_POWER_ON:
        // WinUSB 版の OpenCard と同じく状態だけを作る。カードへの給電は RESET の手順が行う。
        if (!card_powered_) {
            card_powered_ = true;
            card_.Discard();
        }
        return 0;
    case PX4_CARD_POWER_OFF:
        CardPowerOffLocked();
        return 0;
    case PX4_CARD_DETECT: {
        bool present = false;
        const int rc = card_.Detect(channel_, present);
        value = present;
        return rc;
    }
    case PX4_CARD_RESET:
        return card_.Reset(channel_);
    case PX4_CARD_SET_BAUDRATE:
        return card_.SetBaudrate(channel_, value);
    case PX4_CARD_RX_READY: {
        bool ready = false;
        const int rc = card_.DataReady(channel_, ready);
        value = ready;
        return rc;
    }
    case PX4_CARD_READ:
        if (!frame.length)
            return -EINVAL;
        frame.length = static_cast<std::uint8_t>(card_.Read(frame.data, frame.length));
        return 0;
    case PX4_CARD_WRITE:
        return card_.Write(channel_, frame.data, frame.length);
    default:
        return -ENOTTY;
    }
}

void Device::CardIoctl(fuse_req_t req, int cmd, void *, struct fuse_file_info *, unsigned, const void *in,
                       size_t in_size, size_t)
{
    auto &self = Self(req);
    // 引数は固定幅の型だけなので、32 ビットのプロセスも同じ処理で扱える。
    const auto ucmd = static_cast<unsigned>(cmd);
    std::uint32_t value = 0;
    px4_card_frame frame{};
    if (ucmd == PX4_CARD_SET_BAUDRATE) {
        if (in_size < sizeof(value)) {
            fuse_reply_err(req, EFAULT);
            return;
        }
        std::memcpy(&value, in, sizeof(value));
    } else if (ucmd == PX4_CARD_READ || ucmd == PX4_CARD_WRITE) {
        if (in_size < sizeof(frame)) {
            fuse_reply_err(req, EFAULT);
            return;
        }
        std::memcpy(&frame, in, sizeof(frame));
    }
    const int rc = self.CardRequest(ucmd, value, frame);
    if (rc)
        fuse_reply_err(req, -rc);
    else if (ucmd == PX4_CARD_DETECT || ucmd == PX4_CARD_RX_READY)
        fuse_reply_ioctl(req, 0, &value, sizeof(value));
    else if (ucmd == PX4_CARD_READ)
        fuse_reply_ioctl(req, 0, &frame, sizeof(frame));
    else
        fuse_reply_ioctl(req, 0, nullptr, 0);
}
}
