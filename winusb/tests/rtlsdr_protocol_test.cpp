// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/rtlsdr/protocol.hpp"
#include <cstdio>
#include <stdexcept>
#include <array>
#include <vector>
using namespace px4::rtlsdr;
void Require(bool ok){if(!ok)throw std::runtime_error("RTL-SDR protocol assertion failed");}
std::vector<std::uint8_t> Message(std::uint32_t kind,std::uint32_t size)
{
    std::vector<std::uint8_t> out;
    for(auto word:{Magic,kind,size,10000U,1U,0U})for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(word>>(i*8)));
    out.resize(24+size,0xff);
    if(kind==2 && size>=188){out[24]=0x47;out[25]=0;out[26]=0;out[27]=0x10;}
    return out;
}
int main()
{
    try {
        auto packet=Message(2,188);Frames parser;unsigned calls=0;
        for(auto byte:packet)Require(parser.Feed(&byte,1,[&](const Frame &frame,const std::uint8_t *data){Require(frame.kind==2 && frame.size==188 && data[0]==0x47);++calls;return true;}));
        Require(calls==1 && parser.Empty());
        auto ready=Message(1,0);packet.insert(packet.end(),ready.begin(),ready.end());
        calls=0;Require(parser.Feed(packet.data(),packet.size(),[&](const Frame &,const std::uint8_t *){++calls;return true;}));Require(calls==2);
        auto reject=[](std::vector<std::uint8_t> bad){Frames p;Require(!p.Feed(bad.data(),bad.size(),[](const Frame &,const std::uint8_t *){return true;}));};
        auto bad=Message(2,188);bad[0]^=1;reject(bad);
        reject(Message(2,187));reject(Message(1,1));reject(Message(4,1025));
        reject(Message(3,3));reject(Message(3,MaxStatus+4));
        {
            // 状態の payload の MER (dB×100)。空の payload は MER なしとして受け付ける
            auto status=Message(3,4);status[24]=0x33;status[25]=0x0c;status[26]=0;status[27]=0;
            auto empty=Message(3,0);status.insert(status.end(),empty.begin(),empty.end());
            auto range=Message(3,4);range[24]=0x11;range[25]=0x27;range[26]=0;range[27]=0;status.insert(status.end(),range.begin(),range.end());
            Frames p;std::vector<int> mers;
            Require(p.Feed(status.data(),status.size(),[&](const Frame &frame,const std::uint8_t *data){std::uint32_t m=0;mers.push_back(StatusMer(frame,data,m) ? int(m) : -1);return true;}));
            Require(mers==std::vector<int>{3123,-1,-1});
        }
        bad=Message(2,188);bad[24]=0;reject(bad);
        bad=Message(2,188);bad[25]=0x80;reject(bad);
        bad=Message(2,188);bad[27]=0x30;bad[28]=184;reject(bad);
        bad=Message(2,188);bad[12]=0xff;bad[13]=0xff;reject(bad);
        bad=Message(2,188);Frames failure;Require(!failure.Feed(bad.data(),bad.size(),[](const Frame &,const std::uint8_t *){return false;}));
        bad=Message(2,188);bad.pop_back();Frames truncated;Require(truncated.Feed(bad.data(),bad.size(),[](const Frame &,const std::uint8_t *){return true;}));Require(!truncated.Empty());
        std::puts("RTL-SDR framing: split/coalesced/invalid/failed/truncated cases passed");return 0;
    }catch(const std::exception &error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
