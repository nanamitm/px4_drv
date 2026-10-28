// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/pxbcud/device.hpp"
#include "msg.h"
#include <setupapi.h>
#include <vector>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"winusb.lib")

int wmain(int argc,wchar_t **argv)
{
    msg_set_mode(MSG_MODE_CONSOLE);
    const GUID interface_guid={0x07b21ac1,0x70c2,0x4bfc,{0x8c,0x4b,0xe1,0x3f,0xaf,0x28,0xce,0x5a}};
    HDEVINFO list=SetupDiGetClassDevsW(&interface_guid,nullptr,nullptr,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE)return 2;
    SP_DEVICE_INTERFACE_DATA entry{};entry.cbSize=sizeof(entry);
    if(!SetupDiEnumDeviceInterfaces(list,nullptr,&interface_guid,0,&entry)) {
        SetupDiDestroyDeviceInfoList(list);std::puts("PX-BCUD WinUSB interface not found.");return 2;
    }
    DWORD size=0;SetupDiGetDeviceInterfaceDetailW(list,&entry,nullptr,0,&size,nullptr);
    std::vector<unsigned char> data(size);
    auto *detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(data.data());detail->cbSize=sizeof(*detail);
    if(!SetupDiGetDeviceInterfaceDetailW(list,&entry,detail,size,nullptr,nullptr)){SetupDiDestroyDeviceInfoList(list);return 2;}
    std::wstring path=detail->DevicePath;SetupDiDestroyDeviceInfoList(list);
    px4::DeviceDefinition definition{};
    definition.name=L"PLEX PX-BCUD";definition.device_interface_guid=interface_guid;
    definition.receivers.push_back({L"PX-BCUD ISDB-S",{},px4::SystemType::ISDB_S,0});
    px4::ReceiverManager manager;
    try {
        px4::pxbcud::Device device(path,definition,0,manager);
        int ret=device.Init();if(ret){std::printf("Init failed ret=%d\n",ret);return 1;}
        auto *receiver=device.GetReceiver(0);
        ret=receiver->Open();if(ret){std::printf("Open failed ret=%d\n",ret);return 1;}
        struct Close {px4::ReceiverBase *receiver;~Close(){receiver->Close();}} close{receiver};
        std::puts("PX-BCUD bridge and frontend initialization succeeded.");
        if(argc<2)return 0;
        const auto frequency=static_cast<std::uint32_t>(wcstoul(argv[1],nullptr,0));
        const auto stream=argc>2?static_cast<std::uint32_t>(wcstoul(argv[2],nullptr,0)):0;
        const unsigned seconds=argc>3?static_cast<unsigned>(wcstoul(argv[3],nullptr,0)):10;
        if(!seconds || seconds>3600)return 2;
        // BonDriver と同じく初回選局より先に capture を要求する。
        ret=receiver->SetCapture(true);if(ret){std::printf("SetCapture failed ret=%d\n",ret);return 1;}
        px4::command::ParameterSet parameters{};
        parameters.system=px4::SystemType::ISDB_S;parameters.freq=frequency;parameters.num=1;
        parameters.params[0]={px4::command::ParameterType::STREAM_ID,stream};
        if(!receiver->SetParameters(parameters) || !receiver->Tune(5000)){std::puts("Tune failed.");return 1;}
        std::puts("Tune and TSID selection succeeded.");
        auto buffer=receiver->GetStreamBuffer();
        std::atomic<std::uint64_t> packets{0},tei{0},sync{0},cc_errors{0},malformed{0};
        FILE *output=nullptr;
        if(argc>4 && wcscmp(argv[4],L"-") && _wfopen_s(&output,argv[4],L"wb")){receiver->SetCapture(false);std::puts("Output open failed.");return 1;}
        std::atomic_bool write_error{false};
        std::thread reader([&]{
            std::array<int,8192> counters;counters.fill(-1);
            std::uint32_t previous_epoch=0;
            buffer->PrepareRead();
            buffer->HandleReadWithEpoch(188*1024,[&](const void *input,std::size_t bytes,std::uint32_t epoch){
                if (epoch!=previous_epoch) {counters.fill(-1);previous_epoch=epoch;}
                const auto *p=static_cast<const unsigned char*>(input);
                if(output && fwrite(p,1,bytes,output)!=bytes){write_error=true;return false;}
                for(std::size_t i=0;i+188<=bytes;i+=188) {
                    const auto *ts=p+i;++packets;
                    if(ts[0]!=0x47){++sync;continue;}
                    if(ts[1]&0x80)++tei;
                    const unsigned pid=((ts[1]&31)<<8)|ts[2];
                    const unsigned afc=(ts[3]>>4)&3,cc=ts[3]&15;
                    if(!afc || ((afc&2) && ts[4]>(afc==2?183:182))){++malformed;continue;}
                    const bool discontinuity=(afc&2) && ts[4] && (ts[5]&0x80);
                    if(discontinuity)counters[pid]=-1;
                    if(pid!=0x1fff && (afc&1)) {
                        if(counters[pid]>=0 && cc!=static_cast<unsigned>((counters[pid]+1)&15) && cc!=static_cast<unsigned>(counters[pid])) {
                            ++cc_errors;
                            std::printf("CC error epoch=%u pid=0x%04x previous=%d current=%u packet=%llu\n",epoch,pid,counters[pid],cc,static_cast<unsigned long long>(packets.load()));
                        }
                        counters[pid]=cc;
                    }
                }
                return true;
            });
        });
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        bool switch_failed=false;
        if (argc>5) {
            const auto second_frequency=static_cast<std::uint32_t>(wcstoul(argv[5],nullptr,0));
            for (const auto next_frequency:{second_frequency,frequency}) {
                parameters.freq=next_frequency;
                if (!receiver->SetParameters(parameters) || !receiver->Tune(5000)) {
                    switch_failed=true;std::printf("Active channel switch failed frequency_khz=%u\n",next_frequency);break;
                }
                std::printf("Active channel switch succeeded frequency_khz=%u\n",next_frequency);
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
            }
        }
        bool locked=false;const int lock_status=receiver->CheckLock(locked);
        std::int32_t cnr=0;const int cn_status=receiver->ReadStat(px4::command::StatType::CNR,cnr);
        ret=receiver->SetCapture(false);
        buffer->StopRequest();reader.join();
        if(output && fclose(output))write_error=true;
        std::printf("seconds=%u frequency_khz=%u stream_id=%u packets=%llu sync=%llu tei=%llu cc_errors=%llu malformed=%llu locked=%d lock_ret=%d cnr_x1000=%d cn_ret=%d stop_ret=%d write_error=%d\n",
            seconds,frequency,stream,static_cast<unsigned long long>(packets.load()),static_cast<unsigned long long>(sync.load()),
            static_cast<unsigned long long>(tei.load()),static_cast<unsigned long long>(cc_errors.load()),static_cast<unsigned long long>(malformed.load()),
            locked,lock_status,cnr,cn_status,ret,write_error.load());
        return packets && !sync && !tei && !cc_errors && !malformed && locked && !lock_status && !ret && !write_error && !switch_failed?0:1;
    } catch(const std::exception &ex){std::printf("Exception: %s\n",ex.what());return 1;}
}
