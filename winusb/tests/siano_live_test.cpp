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
#pragma comment(lib,"setupapi.lib")
#pragma comment(lib,"winusb.lib")

int wmain(int argc,wchar_t** argv)
{
    if (argc!=4) {std::puts("Usage: siano_live_test channel seconds output.ts");return 2;}
    const unsigned channel=wcstoul(argv[1],nullptr,10),seconds=wcstoul(argv[2],nullptr,10);
    if (channel<13 || channel>62 || !seconds || seconds>3600) return 2;
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
        FILE* raw=nullptr;
        if (_wfopen_s(&raw,argv[3],L"wb")) {std::puts("Output open failed");return 2;}
        std::unique_ptr<FILE,decltype(&fclose)> output(raw,fclose);
        ret=receiver->SetCapture(true);if (ret) {std::printf("Capture failed ret=%d\n",ret);return 1;}
        auto buffer=receiver->GetStreamBuffer();
        std::uint64_t packets=0,sync=0,tei=0,cc_errors=0,malformed=0;
        std::atomic_bool write_error{false};
        buffer->PrepareRead();
        std::thread reader([&] {
            std::array<int,8192> cc;cc.fill(-1);std::uint32_t last_epoch=0;
            buffer->HandleReadWithEpoch(188*1024,[&](const void* data,std::size_t n,std::uint32_t epoch) {
                if (epoch!=last_epoch) {cc.fill(-1);last_epoch=epoch;}
                if (n%188) ++malformed;
                if (fwrite(data,1,n,raw)!=n) {write_error=true;return false;}
                const auto* bytes=static_cast<const std::uint8_t*>(data);
                for (std::size_t i=0;i+188<=n;i+=188) {
                    const auto* p=bytes+i;++packets;
                    if (p[0]!=0x47) {++sync;continue;}
                    if (p[1]&0x80) ++tei;
                    const auto pid=((p[1]&31)<<8)|p[2];const unsigned afc=(p[3]>>4)&3,c=p[3]&15;
                    if (!afc || ((afc&2) && p[4]>(afc==2 ? 183 : 182))) {++malformed;continue;}
                    if ((afc&2) && p[4] && (p[5]&0x80)) cc[pid]=-1;
                    if (pid!=0x1fff && (afc&1)) {
                        if (cc[pid]>=0 && c!=static_cast<unsigned>((cc[pid]+1)&15) && c!=static_cast<unsigned>(cc[pid])) ++cc_errors;
                        cc[pid]=c;
                    }
                }
                return true;
            });
        });
        struct Stop {
            px4::ReceiverBase* r;std::thread& reader;bool stopped=false;
            int Finish() {int ret=r->SetCapture(false);r->GetStreamBuffer()->StopRequest();if (reader.joinable()) reader.join();stopped=true;return ret;}
            ~Stop(){if (!stopped) Finish();}
        } stop{receiver,reader};
        px4::command::ParameterSet parameters{};parameters.system=px4::SystemType::ISDB_T;
        parameters.freq=(channel*6+395)*1000+143;
        std::printf("channel=%u frequency_khz=%u duration_seconds=%u arch_bits=%zu\n",channel,parameters.freq,seconds,sizeof(void*)*8);
        if (!receiver->SetParameters(parameters) || !receiver->Tune(5000)) {std::puts("Tune failed");return 1;}
        std::puts("Tuned and demod locked");
        const auto begin=std::chrono::steady_clock::now();bool failed=false;
        while (std::chrono::steady_clock::now()-begin<std::chrono::seconds(seconds)) {
            bool locked=false;std::int32_t cnr=0;
            const int lock_ret=receiver->CheckLock(locked),cn_ret=receiver->ReadStat(px4::command::StatType::CNR,cnr);
            std::printf("status locked=%d lock_ret=%d cnr_x1000=%d cn_ret=%d\n",locked,lock_ret,cnr,cn_ret);
            if (!locked || lock_ret || cn_ret || write_error) {failed=true;break;}
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        const int stop_ret=stop.Finish();
        if (fflush(raw) || ferror(raw)) write_error=true;
        const auto diagnostics=buffer->GetWriteDiagnostics();
        std::printf("RESULT packets=%llu sync_errors=%llu tei=%llu continuity_errors=%llu malformed=%llu rejected_bytes=%llu purge_bytes=%llu stop_ret=%d write_error=%d\n",
            static_cast<unsigned long long>(packets),static_cast<unsigned long long>(sync),static_cast<unsigned long long>(tei),
            static_cast<unsigned long long>(cc_errors),static_cast<unsigned long long>(malformed),
            static_cast<unsigned long long>(diagnostics.rejected_bytes),static_cast<unsigned long long>(diagnostics.purge_write_discarded_bytes),stop_ret,write_error.load());
        return failed || !packets || sync || tei || cc_errors || malformed || diagnostics.rejected_bytes || stop_ret || write_error ? 1 : 0;
    } catch (const std::exception& e) {std::printf("Failure: %s\n",e.what());return 1;}
}
