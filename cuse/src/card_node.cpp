// SPDX-License-Identifier: GPL-2.0-only
#include "card_node.hpp"
#include "msg.h"
#include <cerrno>
#include <cstring>

namespace px4::cuse {
namespace {
CardNode &Self(fuse_req_t req) { return *static_cast<CardNode *>(fuse_req_userdata(req)); }
}

const cuse_lowlevel_ops CardNode::ops_ = [] {
    cuse_lowlevel_ops ops{};
    ops.open = OnOpen;
    ops.release = OnRelease;
    ops.ioctl = OnIoctl;
    return ops;
}();

CardNode::CardNode(CardBackend &backend, const std::atomic_bool &available, std::string label,
                   std::function<void()> changed)
    : backend_(backend), available_(available), label_(std::move(label)), changed_(std::move(changed))
{
}

int CardNode::Start(const std::string &id)
{
    int error = 0;
    // DEVNAME の '!' は udev が '/' に置き換え、/dev/px4card/<ID> になる。
    session_ = CuseSession::Start("px4card!" + id, ops_, 0, this, error);
    if (session_)
        msg_info("%s: serving /dev/px4card/%s\n", label_.c_str(), id.c_str());
    return error;
}

void CardNode::Stop()
{
    if (session_)
        session_->Stop();
    session_.reset();
}

bool CardNode::InUse()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

void CardNode::Remove()
{
    std::lock_guard<std::mutex> lock(mutex_);
    powered_ = false;
    backend_.CardDiscard();
}

void CardNode::Shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    PowerOffLocked();
}

void CardNode::OnOpen(fuse_req_t req, struct fuse_file_info *fi)
{
    auto &self = Self(req);
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (!self.available_) {
        fuse_reply_err(req, ENODEV);
        return;
    }
    // カーネルと同じく、同時に開けるのは 1 つだけ。
    if (self.open_) {
        fuse_reply_err(req, EBUSY);
        return;
    }
    if (const int rc = self.backend_.CardOpen()) {
        fuse_reply_err(req, -rc);
        return;
    }
    self.open_ = true;
    fi->nonseekable = 1;
    fuse_reply_open(req, fi);
}

void CardNode::OnRelease(fuse_req_t req, struct fuse_file_info *)
{
    auto &self = Self(req);
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.PowerOffLocked();
        self.backend_.CardClose();
        self.open_ = false;
    }
    fuse_reply_err(req, 0);
    if (!self.available_ && self.changed_)
        self.changed_();
}

// 失敗しても電源を切った状態として扱う (カーネルの POWER_OFF と同じ)。
void CardNode::PowerOffLocked()
{
    if (!powered_)
        return;
    powered_ = false;
    backend_.CardDiscard();
    if (!available_)
        return;
    if (const int rc = backend_.CardPowerDown())
        msg_warn("%s: card power down failed result=%d\n", label_.c_str(), rc);
}

int CardNode::Request(unsigned cmd, std::uint32_t &value, px4_card_frame &frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 切断後のコマンドは ENODEV を返す (include/px4_card_ioctl.h)。
    if (!available_)
        return -ENODEV;
    // カーネルの px4_card_request() と同じく、電源投入前の UART の操作は EHOSTDOWN にする。
    if (cmd != PX4_CARD_POWER_ON && cmd != PX4_CARD_POWER_OFF && cmd != PX4_CARD_DETECT && !powered_)
        return -EHOSTDOWN;
    switch (cmd) {
    case PX4_CARD_POWER_ON:
        // WinUSB 版の OpenCard と同じく状態だけを作る。カードへの給電は RESET の手順が行う。
        if (!powered_) {
            powered_ = true;
            backend_.CardDiscard();
        }
        return 0;
    case PX4_CARD_POWER_OFF:
        PowerOffLocked();
        return 0;
    case PX4_CARD_DETECT: {
        bool present = false;
        const int rc = backend_.CardDetect(present);
        value = present;
        return rc;
    }
    case PX4_CARD_RESET:
        return backend_.CardReset();
    case PX4_CARD_SET_BAUDRATE:
        return backend_.CardSetBaudrate(value);
    case PX4_CARD_RX_READY: {
        bool ready = false;
        const int rc = backend_.CardDataReady(ready);
        value = ready;
        return rc;
    }
    case PX4_CARD_READ:
        if (!frame.length)
            return -EINVAL;
        frame.length = static_cast<std::uint8_t>(backend_.CardRead(frame.data, frame.length));
        return 0;
    case PX4_CARD_WRITE:
        return backend_.CardWrite(frame.data, frame.length);
    default:
        return -ENOTTY;
    }
}

void CardNode::OnIoctl(fuse_req_t req, int cmd, void *, struct fuse_file_info *, unsigned, const void *in,
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
    const int rc = self.Request(ucmd, value, frame);
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
