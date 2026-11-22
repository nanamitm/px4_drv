// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "cuse_session.hpp"
#include <sys/ioctl.h>
#include "ptx_ioctl.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace px4::cuse {
// 受信のデバイスファイルの機種ごとの処理。VideoNode が状態のロックを保持して呼ぶ。
// fatal を true にした失敗は、デバイスファイルを閉じるまで以後の操作を -EIO で断る (カーネルの ptx_chrdev と同じ)。
class VideoBackend {
public:
    virtual ~VideoBackend() = default;
    // open のたびに呼ぶ。0 以外を返すと open を失敗させる。
    virtual int VideoOpen() = 0;
    // 受信を止めた後、デバイスファイルを閉じるたびに呼ぶ。
    virtual void VideoClose() = 0;
    // PTX_SET_SYSTEM_MODE で受け付ける方式か。
    virtual bool AcceptsSystem(std::uint32_t system) const = 0;
    virtual int SetChannel(std::uint32_t system, const ptx_freq &freq, bool streaming, bool &fatal) = 0;
    // 拡張 ioctl (PTXT_SET_PARAMS) で受け付ける方式か。カーネルの ptx_chrdev_set_params() と同じく、
    // 既存の PTX_SET_CHANNEL で選べない ISDB-C と J.83B だけが対象で、対応しない機種は false のまま
    virtual bool AcceptsParams(std::uint32_t) const { return false; }
    // PTXT_TUNE。PTXT_SET_PARAMS で受け付けた方式と中心周波数 (kHz) で選局し、ロックを待つ
    virtual int TuneParams(std::uint32_t, std::uint32_t, bool, bool &) { return -ENOSYS; }
    // 選局済みの状態でロックを返す。
    virtual int LockStatus(bool &locked) = 0;
    // PTX_GET_CNR の値 (TC90522 の CNDAT 相当)。VideoNode が 1 秒に 1 回へ間引く。
    virtual int ReadCn(std::uint32_t &raw) = 0;
    // TS を VideoNode::Push() へ渡し始める。VideoNode は呼ぶ前にリングを空にして受け付けを始める。
    virtual int StartStream(bool &fatal) = 0;
    // TS の受け渡しを止める。戻り値は受信中に起きた異常 (読み尽くした後の read が返す値)。
    virtual int StopStream(bool &fatal) = 0;
};

// 受信のデバイスファイル (/dev/<name>N) の機種共通の部分。カーネルモジュールの ptx_chrdev と同じ契約で
// open、read、poll、PTX の ioctl に応答し、TS は機種ごとの処理が Push() で渡すリングに溜める。
class VideoNode {
public:
    using Clock = std::chrono::steady_clock;
    // changed は切断後にデバイスファイルが閉じられたとき、CUSE のスレッドから呼ばれる。
    VideoNode(VideoBackend &backend, const std::atomic_bool &available, std::string label, std::function<void()> changed);
    ~VideoNode() { Stop(); }
    VideoNode(const VideoNode &) = delete;
    VideoNode &operator=(const VideoNode &) = delete;

    // リングを確保する。
    int Init();
    // devname は DEVNAME= の値 (dt295video0 など)。
    int Start(const std::string &devname);
    // ループを終えてデバイスファイルを消す。何度呼んでもよい。
    void Stop();
    bool Started() const { return static_cast<bool>(session_); }
    // デバイスファイルが開かれている。
    bool InUse();

    // 受信中に TS (188 バイトの倍数) を渡す。どのスレッドから呼んでもよい。
    void Push(const std::uint8_t *data, std::size_t size);
    // 受信中の異常を、受信の停止を待たずに利用者の read へ伝える。
    void Fail(int error);
    std::uint64_t Overflow();

    // USB の切断を反映する。受信を止め、待機中の read と poll を起こす。
    // locked は受信を止めた後、状態のロックを保持したまま呼ぶ (機種ごとの状態の破棄)。
    void Remove(const std::function<void()> &locked);
    // デーモンの終了。受信を止めてから、状態のロックを保持したまま locked を呼ぶ (機器の休止)。
    void Shutdown(const std::function<void()> &locked);

private:
    struct PendingRead {
        fuse_req_t req;
        std::size_t size;
    };
    static void OnOpen(fuse_req_t req, struct fuse_file_info *fi);
    static void OnRelease(fuse_req_t req, struct fuse_file_info *fi);
    static void OnRead(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi);
    static void OnPoll(fuse_req_t req, struct fuse_file_info *fi, struct fuse_pollhandle *ph);
    static void OnIoctl(fuse_req_t req, int cmd, void *arg, struct fuse_file_info *fi, unsigned flags,
                        const void *in, size_t in_size, size_t out_size);
    static void ReadInterrupted(fuse_req_t req, void *data);
    static const cuse_lowlevel_ops ops_;

    int SetChannel(const ptx_freq &freq);
    int SetParams(const ptxt_params &params);
    int TuneParams();
    int ReadCn(std::uint32_t &raw);
    // state_mutex_ を保持して呼ぶ。
    int StartStreamLocked();
    void StopStreamLocked();
    void EndRingLocked(int error, std::vector<PendingRead> &replies);
    static void ReplyReads(std::vector<PendingRead> &replies, int error);

    VideoBackend &backend_;
    const std::atomic_bool &available_;
    const std::string label_;
    std::function<void()> changed_;
    std::unique_ptr<CuseSession> session_;

    // デバイスファイルの状態 (state_mutex_ で保護)
    std::mutex state_mutex_;
    bool open_ = false, tuned_ = false, streaming_ = false, failed_ = false;
    std::uint32_t system_ = 0;
    // PTXT_SET_PARAMS で受け付けた方式と中心周波数 (kHz)。params_system_ が 0 なら未設定
    std::uint32_t params_system_ = 0, params_khz_ = 0;
    bool cn_cached_ = false;
    std::uint32_t cn_raw_ = 0;
    Clock::time_point cn_read_at_{};

    // read() へ渡す TS のリング (ring_mutex_ で保護)。容量は WinUSB 版の受信機と同じ。
    std::mutex ring_mutex_;
    std::vector<std::uint8_t> ring_;
    std::size_t head_ = 0, fill_ = 0;
    bool live_ = false;   // START から STOP までの間、TS を受け付ける
    int ring_error_ = 0;  // 0 以外なら、読み尽くした後の read はこの値で失敗する
    std::uint64_t overflow_ = 0;
    std::deque<PendingRead> reads_;
    struct fuse_pollhandle *poll_ = nullptr;
};
}
