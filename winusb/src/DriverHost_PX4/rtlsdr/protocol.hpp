// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace px4::rtlsdr {
constexpr std::uint32_t Magic=0x31545352;
constexpr std::size_t MaxTs=188*2048;
struct Frame { std::uint32_t kind,size,quality,accepted,rejected; };
// 状態 (kind 3) の payload の上限。品質は RS の成功割合で C/N の表示に使えないため、msi2500_frontend と
// RTL-SDR の helper は先頭 4 バイトに復調器の MER (dB×100、0〜10000) を置く。空の payload (MER を測れていない間と古い helper) も受け付け、
// 後から項目を足しても古い DriverHost が拒まないよう、4 の倍数で 64 バイトまでを許す
constexpr std::uint32_t MaxStatus=64;
constexpr std::uint32_t MaxMerX100=10000;
// 状態の payload から MER (dB×100) を取り出す。載っていない、または範囲外なら false
inline bool StatusMer(const Frame &frame,const std::uint8_t *data,std::uint32_t &mer_x100)
{
    if(frame.kind!=3 || frame.size<4) return false;
    const std::uint32_t v=std::uint32_t(data[0])|(std::uint32_t(data[1])<<8)|(std::uint32_t(data[2])<<16)|(std::uint32_t(data[3])<<24);
    if(v>MaxMerX100) return false;
    mer_x100=v;
    return true;
}
class Frames {
public:
    template<class Handler> bool Feed(const std::uint8_t *bytes,std::size_t size,Handler handler)
    {
        if(size>MaxTs || data_.size()>MaxTs+24) return false;
        data_.insert(data_.end(),bytes,bytes+size);
        std::size_t offset=0;
        while(data_.size()-offset>=24) {
            const auto *p=data_.data()+offset;
            auto word=[](const std::uint8_t *v) {return std::uint32_t(v[0])|(std::uint32_t(v[1])<<8)|(std::uint32_t(v[2])<<16)|(std::uint32_t(v[3])<<24);};
            Frame frame{word(p+4),word(p+8),word(p+12),word(p+16),word(p+20)};
            if(word(p)!=Magic || frame.kind<1 || frame.kind>4 || frame.quality>10000) return false;
            if((frame.kind==2 && (!frame.size || frame.size>MaxTs || frame.size%188)) ||
               (frame.kind==1 && frame.size) ||
               (frame.kind==3 && (frame.size>MaxStatus || frame.size%4)) ||
               (frame.kind==4 && (!frame.size || frame.size>1024))) return false;
            if(data_.size()-offset<24+frame.size) break;
            if(frame.kind==2) for(std::size_t i=0;i<frame.size;i+=188) {
                const auto *ts=p+24+i;
                const unsigned afc=(ts[3]>>4)&3;
                if(ts[0]!=0x47 || (ts[1]&0x80) || !afc || ((afc&2) && ts[4]>183)) return false;
            }
            if(!handler(frame,p+24)) return false;
            offset+=24+frame.size;
        }
        if(offset) data_.erase(data_.begin(),data_.begin()+offset);
        return data_.size()<=MaxTs+24;
    }
    bool Empty() const noexcept {return data_.empty();}
private:
    std::vector<std::uint8_t> data_;
};
}
