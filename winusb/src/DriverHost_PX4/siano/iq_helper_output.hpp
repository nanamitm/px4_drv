// SPDX-License-Identifier: GPL-2.0-only
#pragma once
// J.83B の helper (msi2500_frontend serve-s1ud) の標準出力の解釈。WinUSB 版と Linux 版で共有する。
// 形式は RTL-SDR と MSi2500 の helper と同じフレーム (rtlsdr/protocol.hpp)。
#include "../rtlsdr/protocol.hpp"
#include "msg.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace px4::siano {
class HelperOutput final {
public:
    using TsHandler=std::function<bool(const std::uint8_t*,std::size_t)>;
    // 起動ごとに状態を戻す
    void Reset() {frames_=rtlsdr::Frames{};ready_=false;last_ts_=0;quality_=0;mer_x100_=-1;}
    // 標準出力から読んだバイト列を解釈する。形式の異常、helper の報告したエラー、ts の拒否で false を返す。
    // now_ms は TS の到着時刻 (MonotonicMs)。deliver が false の間 (停止中) は TS を渡さない
    bool Feed(const std::uint8_t* bytes,std::size_t size,std::uint64_t now_ms,bool deliver,const TsHandler& ts)
    {
        bool valid=true;
        // Frames::Feed は 1 回の入力を MaxTs までに制限するため、分けて渡す
        for (std::size_t offset=0;valid && offset<size;) {
            const std::size_t part=(std::min)(size-offset,rtlsdr::MaxTs);
            valid=frames_.Feed(bytes+offset,part,[&](const rtlsdr::Frame& frame,const std::uint8_t* data) {
                if (frame.kind==1) ready_=true;
                if (frame.kind==4) {
                    msg_err("PX-S1UD: J.83B helper error: %.*s\n",static_cast<int>(frame.size),reinterpret_cast<const char*>(data));
                    return false;
                }
                if (frame.kind==3) {
                    quality_=frame.quality;
                    // 判定帰還から外れた間など MER が載らない状態は、直前の値を残す。
                    // ロックの判定は TS の到着で行い、MER は表示だけに使うため
                    std::uint32_t mer=0;
                    if (rtlsdr::StatusMer(frame,data,mer)) mer_x100_=static_cast<std::int32_t>(mer);
                }
                if (frame.kind==2) {
                    quality_=frame.quality;last_ts_=now_ms;
                    if (deliver && !ts(data,frame.size)) return false;
                }
                return true;
            });
            offset+=part;
        }
        return valid;
    }
    // 停止の後に表示の値を戻す
    void Clear() {ready_=false;last_ts_=0;quality_=0;mer_x100_=-1;}
    bool Ready() const {return ready_;}
    std::uint64_t LastTs() const {return last_ts_;}
    std::uint32_t Quality() const {return quality_;}
    void ClearQuality() {quality_=0;}
    std::int32_t MerX100() const {return mer_x100_;}
private:
    rtlsdr::Frames frames_;
    std::atomic_bool ready_{false};
    std::atomic<std::uint64_t> last_ts_{0};
    std::atomic<std::uint32_t> quality_{0};
    std::atomic<std::int32_t> mer_x100_{-1};
};
}
