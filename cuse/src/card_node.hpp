// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "cuse_session.hpp"
#include <sys/ioctl.h>
#include "px4_card_ioctl.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace px4::cuse {
// 内蔵カードリーダーの機種ごとの操作。CardNode がカードのロックを保持して呼ぶ。
class CardBackend {
public:
    virtual ~CardBackend() = default;
    // デバイスファイルを開いたとき。0 以外を返すと open を失敗させる。
    virtual int CardOpen() { return 0; }
    // 電源を切った後、デバイスファイルを閉じたときに呼ぶ。
    virtual void CardClose() {}
    // ATR と T=1 の受信途中の状態を破棄する。
    virtual void CardDiscard() = 0;
    virtual int CardDetect(bool &present) = 0;
    virtual int CardReset() = 0;
    virtual int CardSetBaudrate(std::uint32_t baudrate) = 0;
    virtual int CardDataReady(bool &ready) = 0;
    virtual std::size_t CardRead(std::uint8_t *data, std::size_t capacity) = 0;
    virtual int CardWrite(const std::uint8_t *data, std::size_t size) = 0;
    virtual int CardPowerDown() = 0;
};

// カードのデバイスファイル (/dev/px4card/<ID>) の機種共通の部分。include/px4_card_ioctl.h の契約と、
// カーネルモジュールの px4_card と同じ応答 (同時に 1 つだけ、電源投入前の UART の操作は EHOSTDOWN、
// 切断後は ENODEV、close で電源を切る) を受け持つ。
class CardNode {
public:
    CardNode(CardBackend &backend, const std::atomic_bool &available, std::string label, std::function<void()> changed);
    ~CardNode() { Stop(); }
    CardNode(const CardNode &) = delete;
    CardNode &operator=(const CardNode &) = delete;

    // id は DEVNAME の px4card!<ID> の ID。
    int Start(const std::string &id);
    void Stop();
    bool Started() const { return static_cast<bool>(session_); }
    bool InUse();
    // USB の切断を反映する。カードの状態を破棄する。
    void Remove();
    // デーモンの終了。カードの電源を切る。
    void Shutdown();

private:
    static void OnOpen(fuse_req_t req, struct fuse_file_info *fi);
    static void OnRelease(fuse_req_t req, struct fuse_file_info *fi);
    static void OnIoctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
                        const void *in, size_t in_size, size_t out_size);
    static const cuse_lowlevel_ops ops_;

    int Request(unsigned cmd, std::uint32_t &value, px4_card_frame &frame);
    // mutex_ を保持して呼ぶ。
    void PowerOffLocked();

    CardBackend &backend_;
    const std::atomic_bool &available_;
    const std::string label_;
    std::function<void()> changed_;
    std::unique_ptr<CuseSession> session_;
    std::mutex mutex_;
    bool open_ = false, powered_ = false;
};
}
