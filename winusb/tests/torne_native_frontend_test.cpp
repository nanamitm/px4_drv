// Copyright 2026 Till0196
// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/torne/dib8096_native.hpp"
#include "../src/DriverHost_PX4/torne/frontend_wait.hpp"
#include <fstream>
#include <iomanip>
#include <map>
#include <deque>
#include <iostream>
#include <thread>
using namespace px4::torne;
using namespace px4::torne::dibnative;
class FakeBus final:public Bus {
public:
    std::map<std::uint16_t,std::uint16_t> regs{{896,0x01b3},{897,0x8002}};
    std::map<std::uint16_t,std::uint16_t> tuner{{0x1a,0x0a27}};
    std::map<std::uint16_t,std::deque<std::uint16_t>> queues;
    std::ofstream out;
    Time now{1000000000000};
    unsigned count=0;
    bool failIO=false,failWait=false,jobs=false,changed=false;
    std::map<std::uint32_t,std::array<std::uint32_t,2>> kept;
    explicit FakeBus(const std::string& path):out(path){}
    template<class C> void Hex(const C& value){for(std::int64_t i=0;i<Len(value);++i)out<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(value[i]);out<<std::dec;}
    void Locked(){
        queues[1284]={1,0,2};regs[568]=0x3ffe;regs[554]=1;regs[570]=(3<<4)|2;
        regs[493]=1;regs[494]=12;regs[487]=1;regs[488]=3;regs[481]=2;regs[482]=3;
        regs[499]=2;regs[500]=2;regs[505]=1;regs[542]=1<<6;regs[543]=100<<6;regs[390]=65535-50117;
    }
    std::tuple<Slice<std::uint8_t>,Error> WriteRead(std::uint8_t address,Slice<std::uint8_t> data,std::int64_t length) override {
        ++count;
        out<<(length?"R ":"W ")<<unsigned(address)<<" ";Hex(data);
        if(failIO&&count==12){out<<(length?" IOFAIL\n":"\nIOFAIL\n");return {{},MakeError("injected bus failure")};}
        if(address!=0x12&&(address!=0xc0||regs[1025]!=0xc000))throw std::runtime_error("invalid gate/address");
        if(length==0){
            if(address==0x12){if(data.size()!=4)throw std::runtime_error("demod write shape");regs[(data[0]<<8)|data[1]]=(data[2]<<8)|data[3];}
            else{if(data.size()!=3)throw std::runtime_error("tuner write shape");tuner[data[0]]=(data[1]<<8)|data[2];}
            out<<"\n";return {{},nullptr};
        }
        if(length!=2||data.size()!=(address==0x12?2:1))throw std::runtime_error("read shape");
        std::uint16_t value;
        if(address==0x12){auto reg=std::uint16_t((data[0]<<8)|data[1]);value=regs[reg];auto& queue=queues[reg];if(!queue.empty()){value=queue.front();if(queue.size()>1)queue.pop_front();}}
        else value=tuner[data[0]];
        Slice<std::uint8_t> read{std::uint8_t(value>>8),std::uint8_t(value)};out<<" ";Hex(read);out<<"\n";return {read,nullptr};
    }
    MemoryResult RecallTMCC(std::uint32_t key) override {
        if(!jobs)return {{},false,false,nullptr};out<<"Q "<<key<<"\n";
        auto i=kept.find(key);return {i==kept.end()?std::array<std::uint32_t,2>{}:i->second,i!=kept.end(),true,nullptr};
    }
    MemoryResult RememberTMCC(std::uint8_t address,std::uint32_t key) override {
        out<<"M "<<unsigned(address)<<" "<<key<<"\n";
        auto layer=[&](int i){return std::uint32_t(regs[493+i]&15)|std::uint32_t(regs[499+i]&3)<<4|std::uint32_t(regs[481+i]&7)<<6|std::uint32_t(regs[487+i]&3)<<9;};
        std::array<std::uint32_t,2> words{std::uint32_t(regs[570]&0x73)|std::uint32_t(regs[505]&1)<<8|layer(0)<<12,layer(1)|layer(2)<<16};
        auto i=kept.find(key);bool different=changed||(i!=kept.end()&&i->second!=words);changed=false;kept[key]=words;return {words,different,true,nullptr};
    }
    std::tuple<std::int64_t,std::int64_t,bool,Error> Captrim(std::uint8_t address,std::uint16_t lo4,std::uint32_t key,
        std::int64_t start,std::int64_t step,std::int64_t target,std::int64_t write,std::int64_t read) override {
        if(!jobs)return {0,0,false,nullptr};out<<"K "<<unsigned(address)<<" "<<lo4<<" "<<key<<" "<<start<<" "<<step<<" "<<target<<" "<<write<<" "<<read<<"\n";return {41,7,true,nullptr};
    }
    Error Wait(std::int64_t duration) override{out<<"S "<<duration<<"\n";if(failWait)return MakeError("injected wait failure");now=now.Add(duration);return nullptr;}
};
int RunFixture(int argc,char** argv){
    try{
        if(argc==1){
            using Clock=std::chrono::steady_clock;
            HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!cancel)throw std::runtime_error("cancel event");
            auto begin=Clock::now();auto waited=WaitFrontendDelay(cancel,begin+std::chrono::seconds(1),10000000);
            if(waited||Clock::now()-begin<std::chrono::milliseconds(10))throw std::runtime_error("hardware wait duration");
            if(WaitFrontendDelay(cancel,Clock::now()-std::chrono::milliseconds(1),1000000)!=ERROR_TIMEOUT)throw std::runtime_error("expired wait");
            std::thread stopper([cancel](){Sleep(5);SetEvent(cancel);});
            waited=WaitFrontendDelay(cancel,Clock::now()+std::chrono::seconds(1),500000000);stopper.join();
            if(waited!=ERROR_OPERATION_ABORTED)throw std::runtime_error("cancelled wait");CloseHandle(cancel);
            if(Add<std::uint16_t>(65535,1)!=0||Sub<std::uint32_t>(0,1)!=0xffffffffu||Mul<std::int32_t>(INT32_MAX,2)!=-2||Shl<std::uint32_t>(1,32)!=0||Shr<std::int32_t>(-1,40)!=-1)throw std::runtime_error("integer semantics");
            std::array<int,2> array{};bool bounded=false;try{At(array,2);}catch(const std::out_of_range&){bounded=true;}if(!bounded)throw std::runtime_error("array bounds");
            Slice<int> original{1,2};auto shared=original.sub(1,2);shared[0]=3;if(original[1]!=3)throw std::runtime_error("slice ownership");
            for(const auto* mode:{"locked","nosignal","timeout","unknown","iofail","waitfail","cband","jobs","retune"}){
                char* fixture[]={argv[0],const_cast<char*>(mode),const_cast<char*>("NUL")};if(RunFixture(3,fixture))return 1;
            }
            std::cout<<"PASS: native frontend mock states, failures, integer semantics and bounds. No USB access.\n";return 0;
        }
        if(argc!=3)return 2;std::string mode=argv[1];FakeBus bus(argv[2]);
        if(mode=="locked"||mode=="cband"||mode=="jobs"||mode=="retune")bus.Locked();bus.jobs=mode=="jobs";if(mode=="unknown")bus.regs[897]=0x7000;
        bus.failIO=mode=="iofail";bus.failWait=mode=="waitfail";
        NativeFrontend frontend(bus);frontend.SetClock([&](){return bus.now;});
        auto run=[&](std::uint8_t kind,std::uint32_t hz=0){
            std::vector<std::uint8_t> body,response;if(kind==0x11)for(unsigned i=0;i<4;++i)body.push_back(std::uint8_t(hz>>(i*8)));if(kind==0x12)body={0};
            bool ok=frontend.Command(kind,body,response);bus.out<<"C "<<unsigned(kind)<<" "<<unsigned(ok)<<" ";bus.Hex(response);bus.out<<"\n";
            if(kind==0x10){bool failure=mode=="unknown"||mode=="iofail"||mode=="waitfail";if(ok==failure)throw std::runtime_error("initialize outcome");}
            else if(kind==0x11){if(ok==(mode=="timeout"))throw std::runtime_error("tune outcome");if(ok&&((mode=="nosignal")!=(response==std::vector<std::uint8_t>{1})))throw std::runtime_error("search outcome");}
            else if(!ok)throw std::runtime_error("status/output/sleep outcome");
            if(kind==0x13){if(response.size()!=24||bool(response[0])!=(mode=="locked"||mode=="cband"||mode=="jobs"||mode=="retune"))throw std::runtime_error("lock outcome");}
        };
        run(0x10);if(mode=="unknown"||mode=="iofail"||mode=="waitfail")return 0;
        if(mode=="retune") {
            bus.queues[1284]={1,0,1,1,0,2};bus.queues[568]={0,0,0x3ffe};
            std::vector<std::uint8_t> body{0x49,0x9a,0x33,0x1c},response;
            const auto hz=std::uint32_t(473142857);for(unsigned i=0;i<4;++i)body[i]=std::uint8_t(hz>>(8*i));
            if(!frontend.Command(0x11,body,response)||response!=std::vector<std::uint8_t>{1})throw std::runtime_error("first search");bus.out<<"RETRY initial\n";
            if(!frontend.Command(0x13,{},response)||response[0])throw std::runtime_error("premature retry");bus.out<<"RETRY pending\n";
            bus.Wait(1000000000);run(0x13);run(0x14);return 0;
        }
        if(mode=="nosignal")bus.queues[1284]={1,0,1};
        run(0x11,mode=="cband"?93142857:473142857);run(0x13);run(0x12);run(0x14);
        if(mode=="locked"){bus.queues[1284]={1,0,2};run(0x11,213142857);run(0x13);run(0x14);}
        if(mode=="jobs"){run(0x11,473142857);run(0x13);bus.changed=true;run(0x11,473142857);run(0x13);}
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
int main(int argc,char** argv){return RunFixture(argc,argv);}
