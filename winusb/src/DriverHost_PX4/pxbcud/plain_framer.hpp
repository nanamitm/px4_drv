// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <vector>
#include "../../../../driver/ts_sync.h"

namespace px4::pxbcud {
class PlainFramer {
public:
    void Gap() { discarded+=pending_.size();pending_.clear();locked_=false; }
    void Feed(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &output)
    {
        pending_.insert(pending_.end(),data,data+size);
        std::size_t offset=0;
        while (pending_.size()-offset>=188) {
            if (!locked_) {
                if (pending_.size()-offset<188*3) break;
                bool valid=true;
                for (unsigned i=0;i<3;++i) {
                    const auto *p=pending_.data()+offset+i*188;
                    valid=valid && px4_ts_has_plain_sync(p[0]) && !(p[1]&0x80) && (p[3]&0x30);
                }
                if (!valid) { ++offset; ++discarded; continue; }
                locked_=true;
            }
            const auto *p=pending_.data()+offset;
            if (!px4_ts_has_plain_sync(p[0])) { locked_=false; continue; }
            // 同期後の TEI は捨てず、録画側にもエラーとして見える状態を保つ。
            if (p[1]&0x80) ++tei;
            output.insert(output.end(),p,p+188);
            ++packets;
            offset+=188;
        }
        pending_.erase(pending_.begin(),pending_.begin()+offset);
    }
    std::uint64_t packets=0, tei=0, discarded=0;
private:
    bool locked_=false;
    std::vector<std::uint8_t> pending_;
};
}
