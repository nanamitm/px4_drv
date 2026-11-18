// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../atr_length.hpp"
#include <algorithm>
#include <vector>

namespace px4::dt295 {

// UARTから分割して届くATRとT=1応答を保持する。呼び出し側がカードロックを保持し、USBや電源を操作する。
class CardResponse {
public:
    void Reset() {
        bytes_.clear();remaining_=0;atr_=true;edc_=1;
    }
    bool Ready() const {return remaining_!=0;}
    int Append(const std::uint8_t *bytes,std::size_t count) {
        if(bytes_.size()+count>518)return -EMSGSIZE;
        bytes_.insert(bytes_.end(),bytes,bytes+count);
        if(atr_){
            // 既知のATRとの一致ではなく構造から長さを求め、前後の残留バイトも検出する。
            std::size_t length=0;
            const int rc=px4::GetAtrLength(bytes_.data(),bytes_.size(),length);
            if(rc==-EBADMSG)return rc;
            if(!rc && bytes_.size()>length)return -EBADMSG;
            if(!rc && bytes_.size()==length){
                // SmartCardと同じく、T=1を示すTDの次の第3群以降のTCでEDCを決める。
                // TC2はT=0用なのでCRC指定として扱わない。省略時はLRCの1バイト。
                std::size_t at=2;unsigned group=1;
                auto interfaces=bytes_[1]>>4;
                bool t1=false;
                while(interfaces){
                    if(interfaces&1)++at;
                    if(interfaces&2)++at;
                    if(interfaces&4){
                        if(t1 && group>=3)edc_=(bytes_[at]&1)?2u:1u;
                        ++at;
                    }
                    if(!(interfaces&8))break;
                    const auto td=bytes_[at++];
                    t1=(td&15)==1;interfaces=td>>4;++group;
                }
                remaining_=length;
            }
        }else if(bytes_.size()>=3){
            const auto length=std::size_t(3)+bytes_[2]+edc_;
            if(bytes_.size()>=length)remaining_=length;
        }
        return 0;
    }
    std::size_t Read(std::uint8_t *out,std::size_t capacity) {
        const auto count=(std::min)(capacity,remaining_);
        std::copy_n(bytes_.begin(),count,out);
        bytes_.erase(bytes_.begin(),bytes_.begin()+static_cast<std::ptrdiff_t>(count));
        remaining_-=count;
        if(count && atr_ && !remaining_)atr_=false;
        return count;
    }
private:
    std::vector<std::uint8_t> bytes_;
    std::size_t remaining_=0,edc_=1;
    bool atr_=true;
};
}
