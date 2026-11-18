// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../pxbcud/plain_framer.hpp"
#include <atomic>
#include <mutex>

namespace px4::siano {
// helperのTSは整形済み。配信の停止は実行中の書き込みが終わるまで待つ。
class IqOutputGate final {
public:
    void SetEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_=enabled;
    }

    template<class Writer>
    void Write(Writer write)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_) write();
    }

private:
    std::mutex mutex_;
    bool enabled_=false;
};

// USB 要求投入時の世代を保持し、遅れて完了した旧要求を次の選局へ流さない。
class CaptureGate final {
public:
    std::uint64_t Ticket() const { return generation_.load(); }
    void SetEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_=enabled; ++generation_; framer_.Gap();
    }
    template<class Writer>
    bool Feed(std::uint64_t ticket,const std::uint8_t* data,std::size_t size,Writer write)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || ticket!=generation_) return true;
        output_.clear();framer_.Feed(data,size,output_);
        return output_.empty() || write(output_.data(),output_.size());
    }
    struct Counters {std::uint64_t packets,tei,discarded;};
    Counters Diagnostics()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {framer_.packets,framer_.tei,framer_.discarded};
    }
    // Feed の write コールバック内専用。mutex_ を保持した同じスレッドから読むため再ロックしない。
    Counters DiagnosticsInWrite() const noexcept {return {framer_.packets,framer_.tei,framer_.discarded};}
private:
    std::mutex mutex_;
    std::atomic_uint64_t generation_{1};
    bool enabled_=false;
    pxbcud::PlainFramer framer_;
    std::vector<std::uint8_t> output_;
};
}
