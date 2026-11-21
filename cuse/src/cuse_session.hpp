// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#define FUSE_USE_VERSION 31
#include <cuse_lowlevel.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace px4::cuse {
// 1 つの CUSE のデバイスファイル。機器の数が実行中に増減するため、cuse_lowlevel_main は使わず、
// セッションごとに専用のスレッドで fuse_session_loop を回す。
// 終了のシグナル (SIGTERM など) はプロセスの全スレッドで遮断し、シグナル専用のスレッドだけが受ける。
class CuseSession {
public:
    // devname は DEVNAME= の値 (dt295video0、px4card!<ID> など)。udev は '!' を '/' に置き換える。
    // userdata は fuse_req_userdata() で各要求の処理から参照する。
    static std::unique_ptr<CuseSession> Start(const std::string &devname, const cuse_lowlevel_ops &ops,
                                              unsigned flags, void *userdata, int &error);
    ~CuseSession() { Stop(); }
    CuseSession(const CuseSession &) = delete;
    CuseSession &operator=(const CuseSession &) = delete;
    // ループを終えてデバイスファイルを消す。何度呼んでもよい。セッションのスレッド自身からは呼ばない。
    void Stop();
    const std::string &Name() const { return devname_; }
    // Stop() で使うシグナルのハンドラーを設定する。最初のセッションを作る前に main から 1 回呼ぶ。
    static void InstallWakeHandler();

private:
    CuseSession() = default;
    std::string devname_, info_;
    struct fuse_session *session_ = nullptr;
    std::thread thread_;
    std::atomic_bool done_{false};
    bool stopped_ = false;
};
}
