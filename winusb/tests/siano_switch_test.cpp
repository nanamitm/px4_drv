// SPDX-License-Identifier: GPL-2.0-only
#include "siano/device.hpp"
#include "msg.h"
#include <setupapi.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include <filesystem>
#include <mutex>
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"winusb.lib")

int wmain(int argc,wchar_t** argv)
{
    if (argc!=6) {std::puts("Usage: siano_switch_test channel-a channel-b cycles measure-ms output-dir");return 2;}
    const unsigned channels[]={static_cast<unsigned>(wcstoul(argv[1],nullptr,10)),static_cast<unsigned>(wcstoul(argv[2],nullptr,10))};
    const unsigned cycles=wcstoul(argv[3],nullptr,10),measure_ms=wcstoul(argv[4],nullptr,10);
    if (channels[0]<13 || channels[0]>62 || channels[1]<13 || channels[1]>62 || !cycles || cycles>1000 || measure_ms<1000 || measure_ms>60000) return 2;
    const std::filesystem::path directory=argv[5];
    std::filesystem::create_directories(directory);
    msg_set_mode(static_cast<msg_mode>(MSG_MODE_CONSOLE|MSG_MODE_TIMESTAMP));
    const GUID guid={0xba60b2ee,0x5f40,0x40ca,{0xbc,0x5d,0xb4,0xdd,0x0f,0xc5,0x65,0x11}};
    HDEVINFO list=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
    if (list==INVALID_HANDLE_VALUE) return 2;
    SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
    if (!SetupDiEnumDeviceInterfaces(list,nullptr,&guid,0,&entry)) {
        SetupDiDestroyDeviceInfoList(list);std::puts("PX-S1UD WinUSB interface not found");return 2;
    }
    DWORD size=0;SetupDiGetDeviceInterfaceDetailW(list,&entry,nullptr,0,&size,nullptr);
    std::vector<std::uint8_t> detail_buffer(size);
    auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());detail->cbSize=sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(list,&entry,detail,size,nullptr,nullptr)) {SetupDiDestroyDeviceInfoList(list);return 2;}
    const std::wstring path=detail->DevicePath;SetupDiDestroyDeviceInfoList(list);
    px4::DeviceDefinition definition{};definition.name=L"PLEX PX-S1UD";definition.device_interface_guid=guid;
    definition.receivers.push_back({L"PX-S1UD ISDB-T",{},px4::SystemType::ISDB_T,0});
    px4::ReceiverManager manager;
    try {
        px4::siano::Device device(path,definition,0,manager);
        int ret=device.Init();if (ret) {std::printf("Init failed ret=%d\n",ret);return 1;}
        auto* receiver=device.GetReceiver(0);
        ret=receiver->Open();if (ret) {std::printf("Open failed ret=%d\n",ret);return 1;}
        struct Close {px4::ReceiverBase* r;~Close(){r->Close();}} close{receiver};
        ret=receiver->SetCapture(true);if (ret) {std::printf("Capture failed ret=%d\n",ret);return 1;}
        auto buffer=receiver->GetStreamBuffer();
        struct Record {
            std::uint32_t epoch=0; unsigned channel=0;
            std::uint64_t packets=0,sync=0,tei=0,cc_errors=0,malformed=0;
            std::array<int,8192> cc{};
            std::chrono::steady_clock::time_point first;
            FILE* file=nullptr;
            Record() {cc.fill(-1);}
            ~Record(){if (file) fclose(file);}
        };
        std::mutex records_mutex;
        std::vector<std::unique_ptr<Record>> records;
        std::atomic_bool write_error{false};
        buffer->PrepareRead();
        std::thread reader([&] {
            try {
                buffer->HandleReadWithEpoch(188*1024,[&](const void* data,std::size_t n,std::uint32_t epoch) {
                    std::lock_guard<std::mutex> lock(records_mutex);
                    if (records.empty() || epoch!=records.back()->epoch) {
                        if ((!records.empty() && epoch!=records.back()->epoch+1) || records.size()>=cycles) {write_error=true;return false;}
                        auto record=std::make_unique<Record>();
                        record->epoch=epoch;record->channel=channels[records.size()%2];record->first=std::chrono::steady_clock::now();
                        wchar_t name[100]{};swprintf_s(name,L"cycle%03zu_ch%u_epoch%u.ts",records.size(),record->channel,epoch);
                        const auto path=directory/name;
                        if (_wfopen_s(&record->file,path.c_str(),L"wb")) {write_error=true;return false;}
                        records.push_back(std::move(record));
                    }
                    auto& r=*records.back();
                    if (n%188) ++r.malformed;
                    if (fwrite(data,1,n,r.file)!=n) {write_error=true;return false;}
                    const auto* bytes=static_cast<const std::uint8_t*>(data);
                    for (std::size_t i=0;i+188<=n;i+=188) {
                        const auto* p=bytes+i;++r.packets;
                        if (p[0]!=0x47) {++r.sync;continue;}
                        if (p[1]&0x80) ++r.tei;
                        const auto pid=((p[1]&31)<<8)|p[2];const unsigned afc=(p[3]>>4)&3,c=p[3]&15;
                        if (!afc || ((afc&2) && p[4]>(afc==2 ? 183 : 182))) {++r.malformed;continue;}
                        if ((afc&2) && p[4] && (p[5]&0x80)) r.cc[pid]=-1;
                        if (pid!=0x1fff && (afc&1)) {
                            if (r.cc[pid]>=0 && c!=static_cast<unsigned>((r.cc[pid]+1)&15) && c!=static_cast<unsigned>(r.cc[pid])) ++r.cc_errors;
                            r.cc[pid]=c;
                        }
                    }
                    return true;
                });
            } catch (const std::exception& e) {std::printf("Reader failed: %s\n",e.what());write_error=true;}
        });
        struct Stop {
            px4::ReceiverBase* r;std::thread& reader;bool stopped=false;
            int Finish() {int ret=r->SetCapture(false);r->GetStreamBuffer()->StopRequest();if (reader.joinable()) reader.join();stopped=true;return ret;}
            ~Stop(){if (!stopped) Finish();}
        } stop{receiver,reader};
        bool failed=false;std::vector<std::chrono::steady_clock::time_point> tune_starts;
        for (unsigned cycle=0;cycle<cycles;++cycle) {
            const auto start=std::chrono::steady_clock::now();tune_starts.push_back(start);
            const auto channel=channels[cycle%2];
            px4::command::ParameterSet parameters{};parameters.system=px4::SystemType::ISDB_T;
            parameters.freq=(channel*6+395)*1000+143;
            if (!receiver->SetParameters(parameters) || !receiver->Tune(5000)) {std::printf("Tune failed cycle=%u channel=%u\n",cycle,channel);failed=true;break;}
            const auto locked_at=std::chrono::steady_clock::now();
            std::int32_t cn_min=1000000,cn_max=0;
            while (std::chrono::steady_clock::now()-locked_at<std::chrono::milliseconds(measure_ms)) {
                bool locked=false;std::int32_t cnr=0;
                const int lock_ret=receiver->CheckLock(locked),cn_ret=receiver->ReadStat(px4::command::StatType::CNR,cnr);
                if (!locked || lock_ret || cn_ret || write_error) {
                    std::printf("Status failed cycle=%u locked=%d lock_ret=%d cn_ret=%d\n",cycle,locked,lock_ret,cn_ret);failed=true;break;
                }
                cn_min=(std::min)(cn_min,cnr);cn_max=(std::max)(cn_max,cnr);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            {
                std::lock_guard<std::mutex> lock(records_mutex);
                if (records.size()!=cycle+1) {std::printf("Missing/extra stream epoch cycle=%u records=%zu\n",cycle,records.size());failed=true;}
                else {
                    const auto& r=*records.back();
                    const auto tune_ms=std::chrono::duration_cast<std::chrono::milliseconds>(locked_at-start).count();
                    const auto first_ms=std::chrono::duration_cast<std::chrono::milliseconds>(r.first-start).count();
                    std::printf("CYCLE cycle=%u channel=%u epoch=%u tune_ms=%lld first_ts_ms=%lld packets=%llu sync=%llu tei=%llu cc=%llu malformed=%llu cnr_min=%d cnr_max=%d\n",
                        cycle,channel,r.epoch,tune_ms,first_ms,r.packets,r.sync,r.tei,r.cc_errors,r.malformed,cn_min,cn_max);
                    if (!r.packets || r.sync || r.tei || r.cc_errors || r.malformed || first_ms<0) failed=true;
                }
            }
            fflush(stdout);
            if (failed) break;
        }
        const int stop_ret=stop.Finish();
        std::uint64_t total=0,errors=0;
        for (std::size_t i=0;i<records.size();++i) {
            auto& r=*records[i];total+=r.packets;errors+=r.sync+r.tei+r.cc_errors+r.malformed;
            if (fflush(r.file) || ferror(r.file)) write_error=true;
            std::printf("FINAL cycle=%zu channel=%u epoch=%u packets=%llu sync=%llu tei=%llu cc=%llu malformed=%llu\n",
                i,r.channel,r.epoch,r.packets,r.sync,r.tei,r.cc_errors,r.malformed);
        }
        const auto d=buffer->GetWriteDiagnostics();
        std::printf("RESULT cycles=%zu requested=%u packets=%llu errors=%llu rejected_bytes=%llu purge_bytes=%llu stop_ret=%d write_error=%d\n",
            records.size(),cycles,total,errors,d.rejected_bytes,d.purge_write_discarded_bytes,stop_ret,write_error.load());
        return failed || records.size()!=cycles || !total || errors || d.rejected_bytes || stop_ret || write_error ? 1 : 0;
    } catch (const std::exception& e) {std::printf("Failure: %s\n",e.what());return 1;}
}
