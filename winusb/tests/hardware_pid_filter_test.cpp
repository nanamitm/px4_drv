// SPDX-License-Identifier: GPL-2.0-only
#include "hardware_pid_filter.hpp"
#include "command.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
using Filter=px4::HardwarePidFilter;
using Mode=Filter::Mode;
struct Hardware {
    std::array<std::array<int,64>,5> slots{};
    std::array<unsigned,5> mode{};
    unsigned calls=0,fail=0; int latch=0; bool enable=false, fail_all=false;
    Hardware() {for(auto& s:slots)s.fill(-1);}
    int Write(std::uint32_t reg,const std::uint8_t* data,std::size_t n) {
        if (++calls==fail || fail_all) return -EIO;
        if(reg==0xda16){CHECK(n==2);latch=data[0]|(data[1]<<8);}
        if(reg==0xda14) enable=*data!=0;
        constexpr unsigned indexes[]={0xda15,0xda26,0xda2a,0xda2e,0xda80};
        constexpr unsigned modes[]={0xda13,0xda25,0xda29,0xda2d,0xda7f};
        for(unsigned p=0;p<5;++p){
            if(reg==indexes[p]){CHECK(*data<64);slots[p][*data]=enable?latch:-1;}
            if(reg==modes[p]) mode[p]=*data;
        }
        // ラッチ書き込みを別ポートの操作が割り込める時刻へ寄せる。
        std::this_thread::yield(); return 0;
    }
    std::set<int> Pids(unsigned p) {std::set<int>s;for(int v:slots[p])if(v>=0)s.insert(v);return s;}
};
int main(){
    Hardware h;Filter f;auto w=[&](auto a,auto b,auto n){return h.Write(a,b,n);};
    std::uint16_t first[]={0,100,200,200};
    CHECK(!f.Apply(0,Mode::Pass,first,4,w)); CHECK(h.Pids(0)==std::set<int>({0,100,200}));CHECK(h.mode[0]==2);
    unsigned before=h.calls;CHECK(!f.Apply(0,Mode::Pass,first,4,w));CHECK(h.calls==before);
    std::uint16_t second[]={0,100,300};CHECK(!f.Apply(0,Mode::Pass,second,3,w));CHECK(h.calls-before==6);CHECK(h.Pids(0)==std::set<int>({0,100,300}));
    std::uint16_t full[64];for(unsigned i=0;i<64;++i)full[i]=static_cast<std::uint16_t>(1000+i);
    CHECK(!f.Apply(0,Mode::Pass,full,64,w));full[63]=4000;CHECK(!f.Apply(0,Mode::Pass,full,64,w));CHECK(h.Pids(0).count(4000));CHECK(!h.Pids(0).count(1063));
    CHECK(!f.Apply(0,Mode::Off,nullptr,0,w));CHECK(h.mode[0]==0);
    std::uint16_t bad=0x2000;before=h.calls;
    CHECK(f.Apply(5,Mode::Pass,first,1,w)==-EINVAL);CHECK(f.Apply(0,Mode::Pass,&bad,1,w)==-EINVAL);
    CHECK(f.Apply(0,Mode::Pass,nullptr,0,w)==-EINVAL);CHECK(f.Apply(0,Mode::Pass,first,65,w)==-EINVAL);CHECK(h.calls==before);
    // 初回全再構成と更新の各I/Oに故障を挿入し、次の要求で必ず再構成する。
    for(unsigned at=1;at<=206;++at){
        Hardware hw;Filter filter;hw.fail=at;
        auto writer=[&](auto a,auto b,auto n){return hw.Write(a,b,n);};
        int result=filter.Apply(1,Mode::Pass,first,3,writer);
        CHECK(result==-EIO);
        hw.fail=0;CHECK(!filter.Apply(1,Mode::Pass,second,3,writer));CHECK(hw.Pids(1)==std::set<int>({0,100,300}));CHECK(hw.mode[1]==2);
    }
    for(unsigned at=1;at<=6;++at){
        Hardware hw;Filter filter;auto writer=[&](auto a,auto b,auto n){return hw.Write(a,b,n);};
        CHECK(!filter.Apply(1,Mode::Pass,first,3,writer));hw.fail=hw.calls+at;
        CHECK(filter.Apply(1,Mode::Pass,second,3,writer)==-EIO);
        hw.fail=0;CHECK(!filter.Apply(1,Mode::Block,second,3,writer));CHECK(hw.Pids(1)==std::set<int>({0,100,300}));CHECK(hw.mode[1]==0);
    }
    Hardware broken;Filter recover;
    auto broken_writer=[&](auto reg,auto data,auto count){return broken.Write(reg,data,count);};
    CHECK(!recover.Apply(2,Mode::Pass,first,3,broken_writer));
    broken.fail_all=true;CHECK(recover.Apply(2,Mode::Block,second,3,broken_writer)==-EIO);
    broken.fail_all=false;CHECK(!recover.Apply(2,Mode::Pass,second,3,broken_writer));CHECK(broken.Pids(2)==std::set<int>({0,100,300}));
    recover.Invalidate(2);before=broken.calls;CHECK(!recover.Apply(2,Mode::Pass,second,3,broken_writer));CHECK(broken.calls-before>192);
    CHECK(!recover.Apply(2,Mode::Block,nullptr,0,broken_writer));CHECK(broken.Pids(2).empty());
    Hardware shared;Filter concurrent;
    auto run=[&](unsigned port){auto writer=[&](auto a,auto b,auto n){return shared.Write(a,b,n);};for(unsigned i=0;i<100;++i){std::uint16_t pid=static_cast<std::uint16_t>(100+port*100+i);CHECK(!concurrent.Apply(port,Mode::Pass,&pid,1,writer));}};
    std::thread a(run,0),b(run,4);a.join();b.join();CHECK(shared.Pids(0)==std::set<int>({199}));CHECK(shared.Pids(4)==std::set<int>({599}));
    px4::command::PidFilter request{};CHECK(px4::command::ValidPidFilter(request));request.mode=px4::command::PidFilterMode::Pass;CHECK(!px4::command::ValidPidFilter(request));request.count=65;CHECK(!px4::command::ValidPidFilter(request));request.count=1;request.pids[0]=0x2000;CHECK(!px4::command::ValidPidFilter(request));request.pids[0]=0x1fff;CHECK(px4::command::ValidPidFilter(request));request.mode=static_cast<px4::command::PidFilterMode>(99);CHECK(!px4::command::ValidPidFilter(request));
    std::puts("hardware_pid_filter_test: PASS (diff/full table/fault recovery/port concurrency/IPC validation)");
}
