// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/pxbcud/frontend.hpp"
#include "../src/DriverHost_PX4/pxbcud/plain_framer.hpp"
#include "../src/DriverHost_PX4/pxbcud/pat_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <array>
#include <vector>

using namespace px4::pxbcud;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)

struct Mock final : Io {
    std::array<std::uint8_t,256> bridge{},demod{},tuner{};
    std::uint8_t pointer=0,tuner_pointer=0,status=0;
    bool tuner_read=false;
    unsigned calls=0,fail_at=0,short_at=0,waits=0;
    bool cancelled=false;
    Mock() {
        bridge[0x0a]=114; tuner[0]=0x68;tuner[0x0d]=0x40;
        demod[0xce]=0x40;demod[0xcf]=0x10;
    }
    int Wait(unsigned ms) override { waits+=ms;return cancelled?-ECANCELED:0; }
    int Transfer(bool input,std::uint8_t request,std::uint16_t index,
                 std::uint8_t *data,std::size_t size) override {
        ++calls;
        if (cancelled) return -ECANCELED;
        if (calls==fail_at) return -EIO;
        if (calls==short_at) return static_cast<int>(size)-1;
        CHECK(size>0 && size<=64);
        if (!request) {
            if (input) data[0]=(index==5)?status:bridge[index];
            else bridge[index]=data[0];
        } else {
            CHECK(bridge[6]&4);
            CHECK(index==0x2a || index==0x28);
            CHECK(request==2 || request==3);
            if (input) {
                CHECK(request==2);
                if(tuner_read) { CHECK(size==1);data[0]=tuner[tuner_pointer];tuner_read=false; }
                else for(std::size_t i=0;i<size;++i)data[i]=demod[pointer+i];
            } else if(index==0x2a) {
                pointer=data[0];
                if(data[0]==0xfe) {
                    if(size==2 && data[1]==0xc3) tuner_read=true;
                    else {
                        CHECK(size==3 || size==4);CHECK(data[1]==0xc2);
                        tuner_pointer=data[2];
                        if(size==4)tuner[data[2]]=data[3];
                    }
                } else {
                    for(std::size_t i=1;i<size;++i) demod[pointer+i-1]=data[i];
                    if(pointer==0x90){demod[0xe6]=demod[0x8f];demod[0xe7]=demod[0x90];}
                }
            }
        }
        return static_cast<int>(size);
    }
};

void TestFrontend()
{
    Mock io; Frontend frontend(io);
    CHECK(frontend.Initialize()==0); CHECK(frontend.TunerId()==0x68);
    CHECK(io.bridge[0x5f]==0x80);
    CHECK(io.demod[0x1e]==0x10 && io.demod[0x1c]==0x15 && io.demod[0x1f]==4);
    CHECK(frontend.Tune(1049480)==0);
    // BS-1: IF 1049480 kHz -> -500 kHz, 16MHz PLL, N=13 A=5 SD=0x390994。
    CHECK(io.tuner[6]==13 && (io.tuner[7]&15)==5);
    CHECK((io.tuner[9]&0x3f)==0x39 && io.tuner[10]==0x09 && io.tuner[11]==0x94);
    CHECK(frontend.Tune(950499)==-EINVAL && frontend.Tune(2151500)==-EINVAL);
    bool locked=false;CHECK(frontend.Locked(locked)==0 && locked);
    CHECK(frontend.SelectStream(0)==0);
    CHECK(io.demod[0x8f]==0x40 && io.demod[0x90]==0x10);
    CHECK(frontend.SelectStream(65536)==-EINVAL);
    CHECK(frontend.Capture(true)==0 && io.bridge[0x5f]==0x81);
    CHECK(frontend.Sleep()==0 && io.bridge[0x5f]==0x80);
    CHECK(frontend.Tune(1049480)==-EINVAL);
    CHECK(frontend.Wake()==0);
    const unsigned initialization_calls=[] { Mock m;Frontend f(m);CHECK(f.Initialize()==0);return m.calls; }();
    for(unsigned i=1;i<=initialization_calls;++i) {
        Mock failed;failed.fail_at=i;Frontend f(failed);CHECK(f.Initialize()==-EIO);
        Mock short_read;short_read.short_at=i;Frontend s(short_read);CHECK(s.Initialize()==-EIO);
    }
    Mock wrong_chip;wrong_chip.bridge[0x0a]=113;Frontend c(wrong_chip);CHECK(c.Initialize()==-ENODEV);
    Mock wrong_tuner;wrong_tuner.tuner[0]=0x48;Frontend t(wrong_tuner);CHECK(t.Initialize()==-ENODEV);
    Mock nack;nack.status=0x10;Frontend n(nack);CHECK(n.Initialize()==-ENXIO);
    Mock timeout;timeout.status=2;Frontend b(timeout);CHECK(b.Initialize()==-ETIMEDOUT);
    Mock removed;removed.cancelled=true;Frontend r(removed);CHECK(r.Initialize()==-ECANCELED);
    io.tuner[0x0d]=0;CHECK(frontend.Tune(1049480)==-ETIMEDOUT);
    std::puts("frontend: initialization fault injection, chip IDs, tuning, TSID and cancellation passed");
}
void TestFramer()
{
    std::vector<std::uint8_t> packets(188*8,0);
    for(unsigned i=0;i<8;++i) {packets[i*188]=0x47;packets[i*188+3]=0x10;}
    packets[188*4+1]=0x80;
    for(std::size_t split=0;split<=packets.size();++split) {
        PlainFramer f;std::vector<std::uint8_t> out;
        f.Feed(packets.data(),split,out);
        f.Feed(packets.data()+split,packets.size()-split,out);
        CHECK(out==packets && f.packets==8 && f.tei==1 && f.discarded==0);
    }
    PlainFramer f;std::vector<std::uint8_t> out;
    const std::uint8_t junk[]={0x47,0x80,0,0x10,0,0,0};
    f.Feed(junk,sizeof(junk),out);f.Feed(packets.data(),packets.size(),out);
    CHECK(out==packets && f.discarded==sizeof(junk));
    const std::uint8_t lost_sync[sizeof(junk)]={0,0,0,0,0,0,0};
    out.clear();f.Feed(lost_sync,sizeof(lost_sync),out);f.Feed(packets.data(),packets.size(),out);
    CHECK(out==packets && f.discarded==2*sizeof(junk));
    std::puts("framer: all USB split points, junk, reacquisition and retained TEI passed");
}
std::vector<std::array<std::uint8_t,188>> MakePat(std::uint16_t tsid,unsigned programs=1)
{
    const unsigned length=9+4*programs;
    std::vector<std::uint8_t> section={0,static_cast<std::uint8_t>(0xb0|(length>>8)),static_cast<std::uint8_t>(length),
        static_cast<std::uint8_t>(tsid>>8),static_cast<std::uint8_t>(tsid),0xc1,0,0};
    for(unsigned i=0;i<programs;++i) {section.push_back(0);section.push_back(static_cast<std::uint8_t>(i+1));section.push_back(0xe1);section.push_back(0);}
    std::uint32_t crc=0xffffffff;
    for(auto byte:section) {
        crc^=static_cast<std::uint32_t>(byte)<<24;
        for(unsigned bit=0;bit<8;++bit)crc=(crc&0x80000000)?(crc<<1)^0x04c11db7:crc<<1;
    }
    for(int i=3;i>=0;--i)section.push_back(static_cast<std::uint8_t>(crc>>(8*i)));
    std::vector<std::array<std::uint8_t,188>> result;
    std::size_t offset=0;
    while(offset<section.size()) {
        std::array<std::uint8_t,188> packet;packet.fill(0xff);
        packet[0]=0x47;packet[1]=result.empty()?0x40:0;packet[2]=0;packet[3]=static_cast<std::uint8_t>(0x10|result.size());
        std::size_t payload=result.empty()?5:4;if(result.empty())packet[4]=0;
        const auto count=(std::min)(section.size()-offset,188-payload);
        std::memcpy(packet.data()+payload,section.data()+offset,count);offset+=count;result.push_back(packet);
    }
    return result;
}
void TestPatGate()
{
    auto correct=MakePat(0x4010);auto old=MakePat(0x4030);
    PatGate gate(0x4010);CHECK(!gate.Accept(old[0].data()));
    correct[0][3]=0x11;CHECK(gate.Accept(correct[0].data()));
    auto bad=MakePat(0x4010);bad[0][14]^=1;
    PatGate damaged(0x4010);CHECK(!damaged.Accept(bad[0].data()));CHECK(damaged.Accept(correct[0].data()));
    auto long_pat=MakePat(0x4010,50);CHECK(long_pat.size()==2);
    PatGate spanning(0x4010);CHECK(!spanning.Accept(long_pat[0].data()));CHECK(spanning.Accept(long_pat[1].data()));
    PatGate missing(0x4010);CHECK(!missing.Accept(long_pat[0].data()));long_pat[1][3]=0x12;CHECK(!missing.Accept(long_pat[1].data()));
    PatGate invalid(0x4010);bad[0]=correct[0];bad[0][1]|=0x80;CHECK(!invalid.Accept(bad[0].data()));
    bad[0]=correct[0];bad[0][4]=184;CHECK(!invalid.Accept(bad[0].data()));
    std::puts("PAT gate: stale TSID, CRC failure, split sections, lost packets and invalid pointers passed");
}
int main() {TestFrontend();TestFramer();TestPatGate();return 0;}
