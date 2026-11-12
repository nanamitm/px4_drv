// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace px4::dt295 {

// TSリングの未送信量が増えた原因を、増え始める前の制御経路の使われ方から調べるための固定長の履歴。
// 数時間の受信でも記録量が増えないよう古い記録から上書きし、閾値を超えたときだけ呼び出し元が書き出す。
class StallHistory {
public:
    enum class Kind : char {
        Service = 'S', // duration=SERVICEの所要時間、a=送ったブロック数、b=送出後の未送信量
        Card = 'C',    // duration=カード操作が制御ロックを保持した時間 (手順内の待機で手放すまでの1区間)
        Cn = 'N',      // duration=CN読み出しが制御ロックを保持した時間
        Bulk = 'B',    // duration=直前のBulk受信完了からの間隔、a=受信バイト数
    };
    struct Event {
        std::int64_t time_us = 0; // 受信開始からの経過時間 (各区間の開始時刻)
        std::uint32_t duration_us = 0, a = 0, b = 0;
        Kind kind = Kind::Service;
    };

    explicit StallHistory(std::size_t capacity) : events_(capacity ? capacity : 1) {}

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_ = 0;
        wrapped_ = false;
    }

    // SERVICE、カード、Bulkの各スレッドから呼ぶ。配列の確保は構築時だけで、受信中は割り当てない。
    void Record(const Event &event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_[next_] = event;
        if (++next_ == events_.size()) {
            next_ = 0;
            wrapped_ = true;
        }
    }

    // 古い順に並べた写し。書き出しは別スレッドで行い、受信経路はこの複写だけを待つ。
    std::vector<Event> Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Event> copy;
        if (wrapped_) {
            copy.reserve(events_.size());
            copy.insert(copy.end(), events_.begin() + static_cast<std::ptrdiff_t>(next_), events_.end());
        } else {
            copy.reserve(next_);
        }
        copy.insert(copy.end(), events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(next_));
        return copy;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Event> events_;
    std::size_t next_ = 0;
    bool wrapped_ = false;
};

// 未送信量がhigh以上になった時点を起点とし、post_us後に1回だけ書き出しを要求する。
// 未送信量がlow以下へ戻るまで次の起点を作らず、1回の滞留を複数のファイルへ分けない。
// 長時間の受信でファイルが増え続けないよう、1回の受信で書き出す数をmax_dumpsまでに制限する。
class StallTrigger {
public:
    StallTrigger(std::uint32_t high, std::uint32_t low, std::int64_t post_us, unsigned max_dumps)
        : high_(high), low_(low), post_us_(post_us), max_dumps_(max_dumps) {}

    void Reset()
    {
        armed_ = max_dumps_ != 0;
        waiting_ = false;
        dumps_ = 0;
        trigger_us_ = 0;
        peak_ = 0;
    }

    // SERVICEごとに呼ぶ。trueのとき、呼び出し元が履歴を写して書き出す。
    bool OnService(std::int64_t now_us, std::uint32_t pending)
    {
        if (waiting_) {
            peak_ = (std::max)(peak_, pending);
            if (now_us - trigger_us_ < post_us_)
                return false;
            waiting_ = false;
            ++dumps_;
            return true;
        }
        if (armed_) {
            if (pending >= high_) {
                armed_ = false;
                waiting_ = true;
                trigger_us_ = now_us;
                peak_ = pending;
            }
            return false;
        }
        if (pending <= low_ && dumps_ < max_dumps_)
            armed_ = true;
        return false;
    }

    // 受信の停止時に呼ぶ。起点の後post_usを待たずに止まった場合も、その滞留を書き出せるようtrueを返す。
    // 停止の直前に起きた滞留 (カードの切断処理など) を記録から落とさないため。
    bool Flush()
    {
        if (!waiting_)
            return false;
        waiting_ = false;
        ++dumps_;
        return true;
    }

    std::int64_t TriggerTime() const { return trigger_us_; }
    std::uint32_t Peak() const { return peak_; }
    unsigned Dumps() const { return dumps_; }

private:
    const std::uint32_t high_, low_;
    const std::int64_t post_us_;
    const unsigned max_dumps_;
    bool armed_ = true, waiting_ = false;
    unsigned dumps_ = 0;
    std::int64_t trigger_us_ = 0;
    std::uint32_t peak_ = 0;
};

}
