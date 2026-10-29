// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace px4 {
// SET_DATA_ID_FRAMED で選択した接続だけの形式。Windows x86/x64 の LE 固定幅値。
struct StreamFrameHeader {
    // TS と分割 TLV の両方を運ぶため、length は188の倍数に限らないバイト数。
    std::uint32_t magic, epoch, length;
};
static_assert(sizeof(StreamFrameHeader)==12,"stream frame wire size");
inline constexpr std::uint32_t StreamFrameMagic=0x31535850; // PXS1
inline constexpr std::uint32_t StreamFrameMax=188*4096;

class StreamFrameReader {
public:
    std::uint32_t Epoch() const noexcept {return epoch_;}
    template<class Read> bool ReadChunk(void* output,std::size_t& size,Read read) {
        if(!output || !size)return false;
        auto exact=[&](void* target,std::size_t wanted){
            auto* bytes=static_cast<std::uint8_t*>(target);
            for(std::size_t offset=0;offset<wanted;){
                std::size_t count=wanted-offset;
                if(!read(bytes+offset,count) || !count || count>wanted-offset)return false;
                offset+=count;
            }
            return true;
        };
        if(offset_==payload_.size()) {
            StreamFrameHeader header{};
            if(!exact(&header,sizeof(header)) || header.magic!=StreamFrameMagic || !header.epoch ||
                !header.length || header.length>StreamFrameMax)return false;
            payload_.resize(header.length);
            if(!exact(payload_.data(),payload_.size()))return false;
            offset_=0;epoch_=header.epoch;
        }
        size=(std::min)(size,payload_.size()-offset_);
        std::memcpy(output,payload_.data()+offset_,size);offset_+=size;
        return true;
    }
private:
    std::vector<std::uint8_t> payload_;
    std::size_t offset_=0;
    std::uint32_t epoch_=0;
};
}
