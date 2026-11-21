// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "cuse_session.hpp"
#include "dt295/control_channel.hpp"
#include "dt295/source_firmware.hpp"
#include "usb_transport.hpp"
#include <sys/ioctl.h>
#include "ptx_ioctl.h"
#include "px4_card_ioctl.h"
#include <libusb.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace px4::cuse {
// 接続された 1 台の PIX-DT295。受信のデバイスファイル (/dev/dt295videoN) とカードのデバイスファイル
// (/dev/px4card/<ID>) を CUSE で公開し、WinUSB 版と共有する ControlChannel で制御転送を直列化する。
//
// 寿命: 切断 (Remove) の後も、開いている利用者がいる間はセッションを残して -EIO (カードは -ENODEV) を返す。
// 全員が閉じたら changed を呼び、所有者 (main.cpp の Manager) が StopSessions() を呼んでから破棄する。
class Device {
public:
    using Clock = std::chrono::steady_clock;
    // changed は CUSE のセッションのスレッドから呼ばれる。所有者は自分のスレッドへ処理を移してから状態を調べる。
    Device(libusb_device_handle *handle, std::uint8_t bus, std::uint8_t address, std::function<void()> changed);
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    // 組み込みファームウェアを検証し、カードリーダーの ID を求める。
    int Init();
    int StartVideo(unsigned index);
    int StartCard();
    void StopSessions();
    // 切断後、利用者が開いていないセッションだけを止める。開いているセッションは閉じられるまで残す。
    // 戻り値は、カードのセッションを止めたとき true (同じ ID の作成を保留している機器を再開させるため)。
    bool StopUnusedSessions();
    // USB の切断を反映する。受信を止め、待機中の read と poll を起こす。
    void Remove();
    // デーモンの終了。受信を止め、機器を起動前の状態へ戻す。
    void Shutdown();

    bool Removed() const { return !available_; }
    // 受信とカードのどちらのデバイスファイルも開かれていない。
    bool Idle();
    bool HasVideo() const { return static_cast<bool>(video_); }
    bool HasCard() const { return static_cast<bool>(card_session_); }
    unsigned VideoIndex() const { return video_index_; }
    const std::string &CardId() const { return card_id_; }
    libusb_device *UsbDevice() const { return libusb_get_device(handle_); }
    libusb_device_handle *Handle() const { return handle_; }
    std::uint8_t Bus() const { return bus_; }
    std::uint8_t Address() const { return address_; }

private:
    struct PendingRead {
        fuse_req_t req;
        std::size_t size;
    };

    // 受信のデバイスファイル (PTX の ioctl)
    static void VideoOpen(fuse_req_t req, struct fuse_file_info *fi);
    static void VideoRelease(fuse_req_t req, struct fuse_file_info *fi);
    static void VideoRead(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi);
    static void VideoPoll(fuse_req_t req, struct fuse_file_info *fi, struct fuse_pollhandle *ph);
    static void VideoIoctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
                           const void *in, size_t in_size, size_t out_size);
    static void ReadInterrupted(fuse_req_t req, void *data);
    // カードのデバイスファイル (include/px4_card_ioctl.h)
    static void CardOpen(fuse_req_t req, struct fuse_file_info *fi);
    static void CardRelease(fuse_req_t req, struct fuse_file_info *fi);
    static void CardIoctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
                          const void *in, size_t in_size, size_t out_size);
    static const cuse_lowlevel_ops video_ops_, card_ops_;

    int SetChannel(const ptx_freq &freq);
    int ReadCn(std::uint32_t &raw);
    int EnsureInitialized();
    void RestoreLocked();
    int StartStream();
    void StopStream();
    static void LIBUSB_CALL BulkDone(libusb_transfer *transfer);
    void RingPush(const std::uint8_t *data, std::size_t size);
    void EndRingLocked(int error, std::vector<PendingRead> &replies);
    static void ReplyReads(std::vector<PendingRead> &replies, int error);
    int CardRequest(unsigned cmd, std::uint32_t &value, px4_card_frame &frame);
    void CardPowerOffLocked();

    libusb_device_handle *handle_;
    const std::uint8_t bus_, address_;
    std::function<void()> changed_;
    std::atomic_bool available_{true};
    RemovalSignal removal_;
    dt295::SourceFirmware source_{true};
    UsbTransport transport_;
    dt295::ControlChannel channel_;
    std::string card_id_;
    unsigned video_index_ = 0;
    std::unique_ptr<CuseSession> video_, card_session_;

    // 受信のデバイスファイルの状態 (state_mutex_ で保護)
    std::mutex state_mutex_;
    bool open_ = false, initialized_ = false, tuned_ = false, streaming_ = false, failed_ = false;
    bool restore_failed_ = false;
    std::uint32_t system_ = 0;
    bool cn_cached_ = false;
    std::uint32_t cn_raw_ = 0;
    Clock::time_point cn_read_at_{};

    // read() へ渡す TS のリング (ring_mutex_ で保護)。容量は WinUSB 版の受信機と同じ。
    std::mutex ring_mutex_;
    std::vector<std::uint8_t> ring_;
    std::size_t head_ = 0, fill_ = 0;
    bool live_ = false;      // START から STOP までの間、TS を受け付ける
    int ring_error_ = 0;     // 0 以外なら、読み尽くした後の read はこの値で失敗する
    std::uint64_t overflow_ = 0;
    std::deque<PendingRead> reads_;
    struct fuse_pollhandle *poll_ = nullptr;

    // Bulk IN と SERVICE (StartStream から StopStream まで)
    struct Stream {
        std::vector<libusb_transfer *> transfers;
        std::vector<std::vector<std::uint8_t>> buffers;
        std::atomic_bool running{false}, deliver{false}, stop{false};
        std::atomic_int error{0};
        std::mutex mutex;
        std::condition_variable idle;
        int in_flight = 0;
        std::thread service;
        std::unique_ptr<dt295::TsDelivery> delivery;
        Clock::time_point previous_read{};
        std::uint64_t max_read_gap_us = 0;
    } stream_;

    // カードのデバイスファイルの状態 (card_mutex_ で保護)
    std::mutex card_mutex_;
    bool card_open_ = false, card_powered_ = false;
    dt295::CardSession card_;
};
}
