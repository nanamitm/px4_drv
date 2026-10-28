// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace px4::pxbcud {
// USB FIFO 内に残る前の選局の TS は、同期が合っていても配信しない。
// CRC が正しく current_next=1 の PAT が選択 TSID と一致した時点で開く。
class PatGate {
public:
    explicit PatGate(std::uint16_t tsid):tsid_(tsid) {}
    bool Ready() const {return ready_;}
    bool Accept(const std::uint8_t *p)
    {
        if (ready_) return true;
        if(p[0]!=0x47 || (p[1]&0x80) || (p[1]&31) || p[2]) return false;
        const unsigned afc=(p[3]>>4)&3;
        if(!(afc&1))return false;
        std::size_t offset=4;
        if(afc&2) {
            if(p[4]>182)return false;
            if(p[4] && (p[5]&0x80)){size_=0;cc_=-1;}
            offset+=1+p[4];
        }
        const int cc=p[3]&15;
        if(cc_==cc)return false;
        if(cc_>=0 && cc!=((cc_+1)&15))size_=0;
        cc_=cc;
        if(p[1]&0x40) {
            const std::size_t pointer=p[offset++];
            if(pointer>=188-offset){size_=0;return false;}
            offset+=pointer;size_=0;collecting_=true;
        }
        if(!collecting_)return false;
        while(offset<188 && size_<section_.size()) {
            section_[size_++]=p[offset++];
            if(size_<3)continue;
            const std::size_t total=3+((section_[1]&15)<<8)+section_[2];
            if(section_[0] || !(section_[1]&0x80) || total<12 || total>section_.size())break;
            if(size_==total) {
                std::uint32_t crc=0xffffffff;
                for(std::size_t i=0;i<size_;++i) {
                    crc^=static_cast<std::uint32_t>(section_[i])<<24;
                    for(unsigned j=0;j<8;++j)crc=(crc&0x80000000)?(crc<<1)^0x04c11db7:crc<<1;
                }
                ready_=!crc && (section_[5]&1) &&
                    static_cast<std::uint16_t>((section_[3]<<8)|section_[4])==tsid_;
                break;
            }
        }
        if(!ready_ && size_>=3) {
            const std::size_t total=3+((section_[1]&15)<<8)+section_[2];
            if(section_[0] || !(section_[1]&0x80) || total<12 || total>section_.size() || size_>=total) {
                size_=0;collecting_=false;
            }
        }
        return ready_;
    }
private:
    const std::uint16_t tsid_;
    std::array<std::uint8_t,1024> section_{};
    std::size_t size_=0;
    int cc_=-1;
    bool collecting_=false,ready_=false;
};
}
