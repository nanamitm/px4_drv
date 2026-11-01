// SPDX-License-Identifier: GPL-2.0-only
// 受信機を明示指定して baseline / filtered / restored を同じ選局で比較する。
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "cmd_client.hpp"
#include "ipc_names.hpp"
#include "ts_continuity.hpp"
using Clock=std::chrono::steady_clock;
namespace {
std::atomic_bool interrupted{false};
BOOL WINAPI Interrupt(DWORD) {interrupted=true;return TRUE;}
unsigned Number(const std::wstring& text) {
    if(text.empty() || text[0]==L'-') throw std::runtime_error("invalid nonnegative integer");
    std::size_t end=0;auto value=std::stoul(text,&end,(text.size()>2 && text[0]==L'0' && (text[1]==L'x' || text[1]==L'X')) ? 16 : 10);
    if(end!=text.size() || value>0xffffffffUL) throw std::runtime_error("invalid integer");
    return static_cast<unsigned>(value);
}
struct Session {
    px4::CtrlCmdClient control;
    px4::PipeClient data;
    HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread reader;
    std::mutex mutex;
    std::unique_ptr<px4::TsContinuityMonitor> monitor=std::make_unique<px4::TsContinuityMonitor>();
    std::array<std::uint64_t,8192> pids{};
    std::array<bool,8192> retain{};
    std::unique_ptr<px4::TsContinuityMonitor> retained_monitor=std::make_unique<px4::TsContinuityMonitor>();
    std::vector<std::uint8_t> tail;
    std::uint64_t bytes=0;
    bool measuring=false,capturing=false;
    struct RetainedEvent {double seconds;std::string phase;px4::TsContinuityMonitor::Event event;};
    std::vector<RetainedEvent> retained_events;
    Clock::time_point started=Clock::now();
    std::string phase="startup";
    void Stage(const char* name){std::lock_guard<std::mutex> lock(mutex);phase=name;}
    std::atomic_bool read_failed{false};
    ~Session() {
        if(capturing)control.SetCapture(false);
        if(cancel)SetEvent(cancel);
        if(reader.joinable())reader.join();
        // Close はホスト側の既定フィルター復元も実行する。
        control.Close();
        if(cancel)CloseHandle(cancel);
    }
    void StartReader(){
        reader=std::thread([this]{
            std::vector<std::uint8_t> buffer(188*4096);
            while(WaitForSingleObject(cancel,0)!=WAIT_OBJECT_0){
                std::size_t n=0;
                if(!data.Read(buffer.data(),buffer.size(),n,cancel)) {
                    if(WaitForSingleObject(cancel,0)!=WAIT_OBJECT_0)read_failed=true;
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex);
                if(measuring){bytes+=n;monitor->Feed(buffer.data(),n,[](const auto&){});}
                tail.insert(tail.end(),buffer.begin(),buffer.begin()+n);
                std::size_t offset=0;
                while(tail.size()-offset>=188){
                    const auto* packet=tail.data()+offset;
                    if(packet[0]!=0x47){++offset;continue;}
                    const unsigned pid=((packet[1]&31)<<8)|packet[2];
                    if(measuring)++pids[pid];
                    // 全区間で残すPIDだけを追い、意図したPID除去をCC欠落と混同しない。
                    if(retain[pid])retained_monitor->Feed(packet,188,[this](const auto& event){
                        if(retained_events.size()<64)retained_events.push_back({std::chrono::duration<double>(Clock::now()-started).count(),phase,event});
                    });
                    offset+=188;
                }
                tail.erase(tail.begin(),tail.begin()+offset);
            }
        });
    }
    void Wait(unsigned seconds){
        const auto end=Clock::now()+std::chrono::seconds(seconds);
        while(Clock::now()<end){
            if(interrupted)throw std::runtime_error("interrupted");
            if(read_failed)throw std::runtime_error("data pipe disconnected");
            Sleep(20);
        }
    }
    bool RetainedClean(){
        std::lock_guard<std::mutex> lock(mutex);
        const auto& c=retained_monitor->Counts();
        for(const auto& saved:retained_events){const auto& e=saved.event;
            std::printf("retained_event,seconds=%.3f,phase=%s,kind=%s,pid=0x%04X,previous=%d,expected=%d,actual=%d\n",saved.seconds,saved.phase.c_str(),px4::TsContinuityMonitor::Name(e.kind),e.pid,e.previous,e.expected,e.actual);
        }
        std::printf("retained_overall,packets=%llu,cc_jump=%llu,same_cc_changed=%llu,duplicate_run=%llu,adaptation_cc=%llu,TEI=%llu,malformed=%llu\n",
            static_cast<unsigned long long>(c.packets),static_cast<unsigned long long>(c.cc_jump),static_cast<unsigned long long>(c.repeat_changed),
            static_cast<unsigned long long>(c.duplicate_run),static_cast<unsigned long long>(c.adaptation_cc),static_cast<unsigned long long>(c.tei),static_cast<unsigned long long>(c.malformed));
        return c.packets && !c.cc_jump && !c.repeat_changed && !c.duplicate_run && !c.adaptation_cc && !c.tei && !c.malformed;
    }
    bool Measure(const char* label,unsigned seconds,const px4::command::PidFilter* filter){
        // 設定前にパイプやRingBufferへ入ったパケットを計測から除く。TSは連続して読み捨てる。
        Stage(label);
        Wait(2);
        auto start=Clock::now();
        {std::lock_guard<std::mutex> lock(mutex);monitor->Reset();pids.fill(0);bytes=0;measuring=true;}
        Wait(seconds);
        std::lock_guard<std::mutex> lock(mutex);measuring=false;
        const double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        const auto& c=monitor->Counts();
        std::uint64_t outside=0;
        std::set<unsigned> selected;
        if(filter)for(unsigned i=0;i<filter->count;++i)selected.insert(filter->pids[i]);
        for(unsigned pid=0;pid<pids.size();++pid)if(pids[pid]){
            std::printf("pid,%s,0x%04X,%llu\n",label,pid,static_cast<unsigned long long>(pids[pid]));
            if(filter){bool listed=selected.count(pid)!=0;if(filter->mode==px4::command::PidFilterMode::Pass ? !listed : listed)outside+=pids[pid];}
        }
        std::printf("summary,%s,seconds=%.3f,bytes=%llu,Mbps=%.3f,packets=%llu,cc_jump=%llu,same_cc_changed=%llu,duplicate_run=%llu,adaptation_cc=%llu,TEI=%llu,sync=%llu,malformed=%llu,unexpected_pid_packets=%llu\n",
            label,elapsed,static_cast<unsigned long long>(bytes),bytes*8/elapsed/1000000,
            static_cast<unsigned long long>(c.packets),static_cast<unsigned long long>(c.cc_jump),static_cast<unsigned long long>(c.repeat_changed),
            static_cast<unsigned long long>(c.duplicate_run),static_cast<unsigned long long>(c.adaptation_cc),static_cast<unsigned long long>(c.tei),
            static_cast<unsigned long long>(c.sync),static_cast<unsigned long long>(c.malformed),static_cast<unsigned long long>(outside));
        std::fflush(stdout);
        return bytes && !outside && !c.cc_jump && !c.repeat_changed && !c.duplicate_run && !c.adaptation_cc && !c.tei && !c.sync && !c.malformed;
    }
};
}
int wmain(int argc,wchar_t** argv){
    if(argc==1 || (argc==2 && std::wstring(argv[1])==L"--help")){
        std::puts("pid_filter_probe --device-guid {GUID} --index N --system J83B|ISDBT|ISDBS --freq KHz --pids 0,0x100,0x101 [--stream-id N] [--seconds 30] [--block]\nConnects to an already running DriverHost_PX4 (IPC >= 0x00040005). Does not start/stop the host.\nRecords baseline, filtered, restored PID counts / Mbps / CC / TEI / sync. Each phase excludes 2s warmup.\nPass list is literal: include PAT/PMT/PCR/ECM/PSI PIDs needed by the intended recording.\nExit 0: all phases have data and no observed errors; 2: observed errors; 1: setup/I/O failure.");return 0;
    }
    try{
        px4::command::ReceiverInfo info{};info.index=-1;
        unsigned seconds=30;px4::command::ParameterSet params{};
        px4::command::PidFilter filter{};filter.mode=px4::command::PidFilterMode::Pass;
        bool guid=false,index=false;
        for(int i=1;i<argc;++i){
            std::wstring key=argv[i];
            if(key==L"--block"){filter.mode=px4::command::PidFilterMode::Block;continue;}
            if(++i>=argc)throw std::runtime_error("missing option value");
            std::wstring value=argv[i];
            if(key==L"--device-guid"){if(FAILED(CLSIDFromString(value.c_str(),&info.device_guid)) || IsEqualGUID(info.device_guid,GUID_NULL))throw std::runtime_error("invalid device GUID");guid=true;}
            else if(key==L"--index"){auto n=Number(value);if(n>4)throw std::runtime_error("index must be 0..4");info.index=n;index=true;}
            else if(key==L"--freq")params.freq=Number(value);
            else if(key==L"--seconds"){seconds=Number(value);if(!seconds || seconds>86400)throw std::runtime_error("seconds must be 1..86400");}
            else if(key==L"--stream-id"){params.num=1;params.params[0]={px4::command::ParameterType::STREAM_ID,Number(value)};}
            else if(key==L"--system"){
                if(value==L"J83B")info.systems=px4::SystemType::J83B;
                else if(value==L"ISDBT")info.systems=px4::SystemType::ISDB_T;
                else if(value==L"ISDBS")info.systems=px4::SystemType::ISDB_S;
                else throw std::runtime_error("system must be J83B, ISDBT or ISDBS (TSMF not supported)");
            }else if(key==L"--pids"){
                if(filter.count)throw std::runtime_error("--pids may be supplied only once");
                std::size_t pos=0;
                while(true){auto end=value.find(L',',pos);auto pid=Number(value.substr(pos,end-pos));
                    if(pid>0x1fff || filter.count==64)throw std::runtime_error("PID must be 0..8191, at most 64 entries");
                    filter.pids[filter.count++]=static_cast<std::uint16_t>(pid);
                    if(end==std::wstring::npos)break;pos=end+1;
                }
            }else throw std::runtime_error("unknown option");
        }
        if(!guid || !index || !params.freq || info.systems==px4::SystemType::UNSPECIFIED || !filter.count)throw std::runtime_error("device-guid/index/system/freq/pids are required");
        params.system=info.systems;
        SetConsoleCtrlHandler(Interrupt,TRUE);
        Session session;
        if(!session.cancel)throw std::runtime_error("CreateEvent failed");
        auto pipe=std::make_unique<px4::PipeClient>();px4::PipeClient::PipeClientConfig config{false,2000};
        if(!pipe->Connect(px4::ipc::Control,config,nullptr))throw std::runtime_error("control pipe unavailable; start the new DriverHost separately");
        session.control.SetPipe(pipe);
        std::uint32_t version=0;
        if(!session.control.GetVersion(version) || (version>>16)!=4 || version<0x00040005)throw std::runtime_error("host does not support PID filter IPC (need >=0x00040005); do not replace a host during reception");
        if(!session.control.Open(info,info.systems,&info))throw std::runtime_error("requested receiver unavailable or busy");
        if(!session.control.SetParams(params) || !session.control.Tune(5000))throw std::runtime_error("tune failed (check frequency in kHz / antenna / external satellite power)");
        // 先に能力を確認し、既定へ復元する。非対応受信機は計測開始前に検出する。
        std::int32_t result;px4::command::PidFilter restore{};
        if(!session.control.SetPidFilter(filter,result))throw std::runtime_error("SET_PID_FILTER failed, errno="+std::to_string(result));
        if(!session.control.SetPidFilter(restore,result))throw std::runtime_error("initial restore failed, errno="+std::to_string(result));
        if(!session.data.Connect(px4::ipc::Data,config,nullptr))throw std::runtime_error("data pipe unavailable");
        px4::command::DataCmd data{};data.cmd=px4::command::DataCmdCode::SET_DATA_ID;data.data_id=info.data_id;std::size_t written=0;
        if(!session.data.Write(&data,sizeof(data),written) || written!=sizeof(data))throw std::runtime_error("data attach failed");
        if(!session.control.SetCapture(true))throw std::runtime_error("capture start failed");
        for(unsigned pid=0;pid<session.retain.size();++pid){
            bool listed=false;for(unsigned i=0;i<filter.count;++i)if(pid==filter.pids[i])listed=true;
            session.retain[pid]=filter.mode==px4::command::PidFilterMode::Pass ? listed : !listed;
        }
        session.capturing=true;session.StartReader();
        bool clean=session.Measure("baseline",seconds,nullptr);
        session.Stage("setting_filter");
        if(!session.control.SetPidFilter(filter,result))throw std::runtime_error("filter update failed, errno="+std::to_string(result));
        clean=session.Measure("filtered",seconds,&filter)&&clean;
        session.Stage("restoring");
        if(!session.control.SetPidFilter(restore,result))throw std::runtime_error("restore failed, errno="+std::to_string(result));
        clean=session.Measure("restored",seconds,nullptr)&&clean;
        clean=session.RetainedClean()&&clean;
        return clean?0:2;
    }catch(const std::exception& e){std::fprintf(stderr,"pid_filter_probe: %s\n",e.what());return 1;}
}
