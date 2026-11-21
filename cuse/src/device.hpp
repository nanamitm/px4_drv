// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "card_node.hpp"
#include "usb_transport.hpp"
#include "video_node.hpp"
#include <libusb.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace px4::cuse {
// USB のシリアル番号を持たない機種のカードリーダーの ID を、USB の接続位置 (バス番号とポートの経路) から求める。
// usb_id は "054c:04b2" のような VID:PID。同じポートへ挿し直すと同じ ID になり、Windows 版の ID
// (デバイスインスタンスのパスから求める) とは一致しない。
bool PortCardId(libusb_device_handle *handle, const char *usb_id, std::string &id);

// 接続された 1 台の機器。受信のデバイスファイル (/dev/<prefix>N) とカードのデバイスファイル
// (/dev/px4card/<ID>) を CUSE で公開し、機種ごとの処理は派生クラスが VideoBackend と CardBackend で受け持つ。
//
// 寿命: 切断 (Remove) の後も、開いている利用者がいる間はセッションを残して -EIO (カードは -ENODEV) を返す。
// 全員が閉じたら changed を呼び、所有者 (main.cpp の Manager) が StopSessions() を呼んでから破棄する。
// 派生クラスのデストラクターは、自分のメンバーを解放する前に StopSessions() を呼ぶ。
class Device : protected VideoBackend, protected CardBackend {
public:
    using Clock = std::chrono::steady_clock;
    // model はログの機種名 (PIX-DT295 など)、prefix は受信のデバイスファイルの名前 (dt295video など)。
    // changed は CUSE のセッションのスレッドから呼ばれる。所有者は自分のスレッドへ処理を移してから状態を調べる。
    Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, const char *model,
           const char *prefix, std::function<void()> changed);
    virtual ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    // 機種ごとの確認とカードリーダーの ID を求め、リングを確保する。
    int Init();
    int StartVideo(unsigned index);
    int StartCard();
    void StopSessions();
    // 切断後、利用者が開いていないセッションだけを止める。開いているセッションは閉じられるまで残す。
    // 戻り値は、カードのセッションを止めたとき true (同じ ID の作成を保留している機器を再開させるため)。
    bool StopUnusedSessions();
    // USB の切断を反映する。受信を止め、待機中の read と poll を起こす。
    void Remove();
    // デーモンの終了。受信を止め、機器を休止させる。
    void Shutdown();

    bool Removed() const { return !available_; }
    // 受信とカードのどちらのデバイスファイルも開かれていない。
    bool Idle();
    bool HasVideo() const { return video_.Started(); }
    bool HasCard() const { return card_.Started(); }
    unsigned VideoIndex() const { return video_index_; }
    const std::string &VideoPrefix() const { return prefix_; }
    const std::string &CardId() const { return card_id_; }
    const std::string &Label() const { return label_; }
    libusb_device *UsbDevice() const { return libusb_get_device(handle_); }
    libusb_device_handle *Handle() const { return handle_; }
    std::uint8_t Bus() const { return bus_; }
    std::uint8_t Address() const { return address_; }

protected:
    // 機種ごとの確認。card_id_ を設定できない機種ではカードのデバイスファイルを作らない。
    virtual int InitModel() = 0;
    // 切断の後、受信を止めてから呼ぶ。USB の転送は行わない。
    virtual void OnRemove() = 0;
    // デーモンの終了の手順で、受信を止めてから呼ぶ。
    virtual void OnShutdown() = 0;

    libusb_device_handle *const handle_;
    const std::uint8_t bus_, address_;
    const std::string label_, prefix_;
    std::atomic_bool available_{true};
    RemovalSignal removal_;
    std::string card_id_;
    VideoNode video_;
    CardNode card_;

private:
    unsigned video_index_ = 0;
};
}
