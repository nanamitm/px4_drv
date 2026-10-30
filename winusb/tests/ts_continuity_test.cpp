// SPDX-License-Identifier: GPL-2.0-only
#include "ts_diagnostic_log.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

static std::vector<std::string> logs;
static std::atomic_bool block_writer{false},writer_entered{false};
extern "C" int msg_printf(const char* format,...)
{
    if (block_writer) {writer_entered=true;while (block_writer) std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    char text[2048];va_list args;va_start(args,format);const auto n=vsnprintf(text,sizeof(text),format,args);va_end(args);
    logs.emplace_back(text);return n;
}
static void Check(bool ok,const char* what) {if (!ok) {std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
using Monitor=px4::TsContinuityMonitor;
using Packet=std::array<std::uint8_t,188>;
static Packet Make(unsigned pid,unsigned cc)
{
    Packet p{};p.fill(0xff);p[0]=0x47;p[1]=static_cast<std::uint8_t>(pid>>8);p[2]=static_cast<std::uint8_t>(pid);
    p[3]=static_cast<std::uint8_t>(0x10|cc);return p;
}
static void Tests()
{
    auto m=std::make_unique<Monitor>();std::vector<Monitor::Event> events;
    auto report=[&](auto e) {events.push_back(e);};
    auto feed=[&](const Packet& p) {m->Feed(p.data(),p.size(),report);};
    feed(Make(100,14));feed(Make(200,7));feed(Make(100,15));feed(Make(100,0));
    Check(events.empty(),"per-PID counters and 15-to-0 wrap");
    auto p=Make(100,1);m->Feed(p.data(),19,report);m->Feed(p.data()+19,169,report);
    Check(events.empty() && m->Counts().packets==5,"fragmented input is preserved");
    feed(Make(100,4));Check(events.size()==1 && events.back().kind==Monitor::Kind::CcJump && events.back().previous==1 && events.back().expected==2 && events.back().actual==4,"missing packets carry PID and before/after CC");
    m->Reset();events.clear();feed(Make(100,12));Check(events.empty(),"purge/epoch resets baseline");
    p=Make(100,5);p[3]=0x35;p[4]=1;p[5]=0x80;feed(p);feed(Make(100,6));
    Check(events.empty(),"discontinuity indicator permits counter change");
    m->Reset();events.clear();p=Make(100,2);feed(p);feed(p);
    Check(events.empty() && m->Counts().duplicates==1,"one identical retransmission is valid");
    feed(p);Check(events.size()==1 && events.back().kind==Monitor::Kind::DuplicateRun,"three identical packets are distinguished");
    m->Reset();events.clear();feed(p);p[10]^=1;feed(p);
    Check(events.size()==1 && events.back().kind==Monitor::Kind::RepeatChanged,"same CC with changed payload detected");
    m->Reset();events.clear();p=Make(100,2);p[3]=0x32;p[4]=7;p[5]=0x10;feed(p);p[7]^=1;feed(p);
    Check(events.empty() && m->Counts().duplicates==1,"PCR-only retransmission update is valid");
    m->Reset();events.clear();feed(Make(100,2));p=Make(100,2);p[3]=0x22;p[4]=183;p[5]=0;feed(p);feed(Make(100,3));
    Check(events.empty(),"adaptation-only packet does not advance CC");
    p[3]=0x25;feed(p);Check(events.back().kind==Monitor::Kind::AdaptationCc,"adaptation-only counter change detected");
    m->Reset();events.clear();feed(Make(8191,2));feed(Make(8191,9));feed(Make(8191,9));Check(events.empty(),"null PID counter ignored");
    p=Make(100,2);p[1]|=0x80;feed(p);feed(Make(100,10));
    Check(events.size()==1 && events.back().kind==Monitor::Kind::Tei && m->Counts().tei==1,"TEI logged without cascading CC errors");
    m->Reset();events.clear();p=Make(100,1);p[3]=0x31;p[4]=183;feed(p);
    Check(events.size()==1 && events.back().kind==Monitor::Kind::Malformed,"adaptation length validated");
    p=Make(100,1);p[0]=0;feed(p);Check(events.back().kind==Monitor::Kind::Sync,"sync error logged");
    m->Reset();events.clear();std::vector<std::uint8_t> s(5,0);
    for (unsigned i=0;i<4;++i) {const auto q=Make(100,i);s.insert(s.end(),q.begin(),q.end());}
    m->Feed(s.data(),s.size(),report);
    Check(events.size()==1 && events[0].kind==Monitor::Kind::Sync && m->Counts().resync_bytes==5 && m->Counts().packets==4,"misaligned start resynchronizes");
    m->Reset();events.clear();s.clear();
    for (unsigned i=0;i<6;++i) {const auto q=Make(100,i);s.insert(s.end(),q.begin(),q.end());}
    s.erase(s.begin()+2*188+10,s.begin()+2*188+110);
    for (std::size_t i=0;i<s.size();i+=50) m->Feed(s.data()+i,(std::min<std::size_t>)(50,s.size()-i),report);
    Check(events.size()==1 && events[0].kind==Monitor::Kind::Sync && m->Counts().sync==1 && m->Counts().resync_bytes==88,
        "mid-stream byte loss is one sync event and CC analysis resumes");

    px4::TsDiagnosticLog logger("test");logger.Enable(true);logs.clear();p=Make(100,0);
    for (unsigned i=0;i<100;++i) {p=Make(100,(i*2)&15);logger.Feed(p.data(),188,1);}
    Check(logger.Counts().cc_jump==99,"all errors counted even when detail lines are omitted");
    logger.Reset();logger.Flush();unsigned details=0;bool suppressed=false,dropped=false;
    for (const auto& line:logs) {if (line.find("TS anomaly")!=std::string::npos) ++details;if (line.find("suppressed=67")!=std::string::npos) suppressed=true;if (line.find("queue_dropped=")!=std::string::npos) dropped=true;}
    Check(details<=32 && (suppressed || dropped),"detail rate limit and explicit queue omission counters");
    logs.clear();logger.Feed(p.data(),188,2);p[3]=0x1b;logger.Feed(p.data(),188,3);logger.Flush();
    for (const auto& line:logs) Check(line.find("TS anomaly")==std::string::npos,"new epoch does not create false loss");
    for (const auto& line:logs) Check(line.find("framer_discarded=-1")!=std::string::npos || line.find("start")!=std::string::npos,"unmeasured source is -1");
    logs.clear();logger.Source(10);p=Make(100,0);logger.Feed(p.data(),188,4);logger.Source(25);p=Make(100,3);logger.Feed(p.data(),188,4);
    logger.Reset();logger.Flush();bool anomaly_source=false,summary_source=false;
    for (const auto& line:logs) {
        if (line.find("framer_discarded=15")==std::string::npos) continue;
        if (line.find("TS anomaly")!=std::string::npos) anomaly_source=true;
        if (line.find("reason=reset")!=std::string::npos) summary_source=true;
    }
    Check(anomaly_source && summary_source,"upstream discards are reported relative to the epoch start");
    logger.Reset();logger.Flush();logger.Enable(false);logs.clear();logger.Feed(p.data(),188,1);Check(logs.empty(),"logging disabled is a no-op");
    logger.Enable(true);block_writer=true;writer_entered=false;logger.Feed(p.data(),188,1);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!writer_entered && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    const bool entered=writer_entered;
    auto producer=std::async(std::launch::async,[&] {for (unsigned i=2;i<400;++i) logger.Feed(p.data(),188,i);});
    const bool nonblocking=producer.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    block_writer=false;producer.get();logger.Flush();
    Check(entered && nonblocking,"blocked log sink cannot stall the TS producer");
    dropped=false;for (const auto& line:logs) if (line.find("queue_dropped=")!=std::string::npos) dropped=true;
    Check(dropped,"full diagnostic queue reports omissions");

    px4::TsDiagnosticLog trace_log("trace");trace_log.Enable(true);
    auto count=[&](const char* text) {unsigned n=0;for (const auto& line:logs) if (line.find(text)!=std::string::npos) ++n;return n;};
    {
        px4::TsTransferTracer tracer;logs.clear();
        for (unsigned i=0;i<30;++i) {px4::TsTransferTrace t{};t.bytes=i;tracer.Begin(t).packets=i;tracer.End(trace_log,i==20,1000);}
        trace_log.Flush();
        Check(count("TS transfer")==24 && count("tag=before")==15 && count("tag=trigger")==1 && count("tag=after")==8,
            "anomaly dumps 15 earlier transfers, the trigger and 8 later transfers");
        Check(count("seq=5 ")==1 && count("tag=trigger seq=20 ")==1 && count("seq=28 ")==1 && !count("seq=29 "),
            "trace window is ordered around the anomalous transfer");
    }
    {
        px4::TsTransferTracer tracer;logs.clear();
        for (unsigned i=0;i<100;++i) {tracer.Begin(px4::TsTransferTrace{});tracer.End(trace_log,true,5000);}
        tracer.Begin(px4::TsTransferTrace{});tracer.End(trace_log,true,6000);trace_log.Flush();
        constexpr unsigned budget=static_cast<unsigned>(px4::TsTransferTracer::Budget);
        Check(count("TS transfer")==budget+1 && count("skipped=")==budget+1 && count("skipped=0")==budget && count("skipped=36")==1,
            "continuous anomalies are capped per second and the omission is reported");
    }
}
int wmain(int argc,wchar_t** argv)
{
    Tests();std::puts("PASS: TS continuity, chunk boundaries, epochs, duplicates/PCR, TEI, adaptation and rate limits");
    if (argc==2) {
        std::ifstream f(std::filesystem::path(argv[1]),std::ios::binary);Check(static_cast<bool>(f),"open sample TS");
        auto m=std::make_unique<Monitor>();std::array<char,65536> b{};
        while (f.read(b.data(),b.size()) || f.gcount()) m->Feed(b.data(),static_cast<std::size_t>(f.gcount()),[](auto){});
        const auto& c=m->Counts();
        std::printf("SAMPLE packets=%llu cc_jump=%llu same_cc_changed=%llu duplicate_run=%llu adaptation_cc=%llu tei=%llu sync=%llu resync_bytes=%llu malformed=%llu\n",
            c.packets,c.cc_jump,c.repeat_changed,c.duplicate_run,c.adaptation_cc,c.tei,c.sync,c.resync_bytes,c.malformed);
    }
}
