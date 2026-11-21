// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <chrono>
#include <cstdint>

namespace px4::torne {
// USB の抜去と受信機の停止要求を、フロントエンドの I2C 要求の合間と待機へ伝える。
// WinUSB 版 (DriverHost_PX4) はイベント、Linux 版 (CUSE のデーモン) は条件変数で実装する。
// 停止要求は受信の開始と再選局のように受信機が中断できる要求だけが見る (stoppable)。
// 抜去は常に見る。
class FrontendSignal {
public:
    enum class State { clear, removed, stopped, failed };
    virtual ~FrontendSignal() = default;
    // 待たずに状態を返す。抜去と停止が両方起きている場合は抜去を返す。
    // failed のときは error に OS のエラーを入れる。
    virtual State Poll(bool stoppable, int &error) noexcept = 0;
    // 抜去を見ずに、受信機の停止要求だけを確かめる。
    virtual bool StopRequested() noexcept = 0;
    // ns だけ待つ。操作全体の期限 deadline を越えては待たない。
    // 待ち終えたら 0、抜去か停止で中断したら Aborted() が真になる値、期限切れと失敗は OS のエラーを返す。
    virtual int Sleep(bool stoppable, std::chrono::steady_clock::time_point deadline, std::int64_t ns) noexcept = 0;
    virtual bool Aborted(int error) const noexcept = 0;
    // 抜去か停止で中断したことを表す OS のエラー (Windows は ERROR_OPERATION_ABORTED)。
    virtual int AbortError() const noexcept = 0;
};
} // namespace px4::torne
