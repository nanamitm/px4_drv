#include <array>
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

void InjectPxBcudCompletionFailure(DWORD error,unsigned count=1);
DWORD ObservedPxBcudCompletionFailure();
unsigned ObservedPxBcudFailureCount();
unsigned ObservedPxBcudStreamCount();

int wmain(int argc,wchar_t **argv)
{
    msg_set_mode(static_cast<msg_mode>(MSG_MODE_CONSOLE|MSG_MODE_TIMESTAMP));
#ifdef PXBCUD_DIAGNOSTIC_TOOL
    const bool diagnostic=true;
    if (argc>5) {std::puts("Usage: pxbcud_usb_diagnostic frequency_khz slot_or_tsid seconds [ts_output_or_-]");return 2;}
#else
    const bool diagnostic=false;
#endif
    if (diagnostic) {
        SYSTEMTIME time{};GetLocalTime(&time);
        wchar_t log_path[128]{};
        swprintf_s(log_path,L"PX-BCUD-USB-%04u%02u%02u-%02u%02u%02u.log",
            time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
        if (!msg_open_file(log_path)) {std::puts("Cannot create diagnostic log.");return 2;}
        msg_set_mode(static_cast<msg_mode>(MSG_MODE_CONSOLE|MSG_MODE_LOG_FILE|MSG_MODE_TIMESTAMP));
        msg_info("PX-BCUD diagnostic auto_recovery=0 arch_bits=%u\n",static_cast<unsigned>(sizeof(void*)*8));
        std::printf("Diagnostic log: %ls\n",log_path);
    }
    const bool recovery_test=argc>5 && !wcscmp(argv[5],L"--recover");
    const bool auto_test=argc>5 && !wcscmp(argv[5],L"--auto-recover");
    const bool limit_test=argc>5 && !wcscmp(argv[5],L"--auto-limit");
    const bool stop_test=argc>5 && !wcscmp(argv[5],L"--auto-stop");
    if (argc>5 && argv[5][0]==L'-' && !recovery_test && !auto_test && !limit_test && !stop_test) return 2;
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
        px4::pxbcud::Device device(path,definition,0,manager,!diagnostic && !recovery_test);
        int ret=device.Init();if(ret){std::printf("Init failed ret=%d\n",ret);return 1;}
        auto *receiver=device.GetReceiver(0);
        ret=receiver->Open();if(ret){std::printf("Open failed ret=%d\n",ret);return 1;}
        struct Close {px4::ReceiverBase *receiver;~Close(){receiver->Close();}} close{receiver};
        std::puts("PX-BCUD bridge and frontend initialization succeeded.");
        if(argc<2)return 0;
        const auto frequency=static_cast<std::uint32_t>(wcstoul(argv[1],nullptr,0));
        const auto stream=argc>2?static_cast<std::uint32_t>(wcstoul(argv[2],nullptr,0)):0;
        const unsigned seconds=argc>3?static_cast<unsigned>(wcstoul(argv[3],nullptr,0)):10;
        if(!seconds || seconds>86400)return 2;
        if (diagnostic) msg_info("PX-BCUD diagnostic frequency_khz=%u stream_id=%u duration_seconds=%u\n",frequency,stream,seconds);
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
        std::atomic_bool reader_exited{false};
        std::atomic<std::uint32_t> last_epoch{0};
        std::thread reader([&]{
            std::array<int,8192> counters;counters.fill(-1);
            std::uint32_t previous_epoch=0;
            buffer->PrepareRead();
            buffer->HandleReadWithEpoch(188*1024,[&](const void *input,std::size_t bytes,std::uint32_t epoch){
                if (epoch!=previous_epoch) {counters.fill(-1);previous_epoch=epoch;}
                last_epoch=epoch;
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
            reader_exited=true;
        });
        if (diagnostic) {
            std::puts("Diagnostic: auto recovery disabled; first failure preserves USB state in the log.");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
            while (std::chrono::steady_clock::now()<deadline) {
                if (device.LastStreamError()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } else std::this_thread::sleep_for(std::chrono::seconds(seconds));
        bool switch_failed=false;
        bool limit_observed=false;
        bool stop_observed=false;
        if (stop_test) {
            InjectPxBcudCompletionFailure(ERROR_GEN_FAILURE,4);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (!ObservedPxBcudFailureCount() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::atomic_bool stopped{false};
            std::thread watchdog([&] {
                for (unsigned i=0;i<5000 && !stopped;++i) Sleep(1);
                if (!stopped) {std::puts("Capture stop deadlocked during recovery.");ExitProcess(1);}
            });
            const int stop_ret=receiver->SetCapture(false);
            stopped=true;watchdog.join();
            stop_observed=!stop_ret && ObservedPxBcudFailureCount()>0;
            std::printf("Auto recovery cancellation observed=%d stop_ret=%d\n",stop_observed,stop_ret);
            if (!stop_observed) switch_failed=true;
        } else if (limit_test) {
            const unsigned before_streams=ObservedPxBcudStreamCount();
            InjectPxBcudCompletionFailure(ERROR_GEN_FAILURE,4);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (ObservedPxBcudFailureCount()<4 && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            bool locked=false;
            const unsigned restarts=ObservedPxBcudStreamCount()-before_streams;
            limit_observed=ObservedPxBcudFailureCount()==4 && restarts==3 && receiver->CheckLock(locked)==-EIO && !reader_exited;
            std::printf("Auto recovery limit observed=%d injected_failures=%u restarted_streams=%u original_reader_active=%d\n",
                limit_observed,ObservedPxBcudFailureCount(),restarts,!reader_exited.load());
            if (!limit_observed) switch_failed=true;
        } else if (recovery_test || auto_test) {
            // 同じ reader とデータ接続のまま二種類の完了エラーから復帰する。
            for (const DWORD error:{ERROR_GEN_FAILURE,ERROR_INVALID_PARAMETER}) {
                const auto before=packets.load();
                const auto epoch_before=last_epoch.load();
                if (!before || reader_exited) {switch_failed=true;break;}
                InjectPxBcudCompletionFailure(error);
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                int status=0;bool locked=false;
                do {
                    status=receiver->CheckLock(locked);
                    if ((auto_test && ObservedPxBcudCompletionFailure()==error) || (!auto_test && status)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } while (std::chrono::steady_clock::now()<deadline);
                if (ObservedPxBcudCompletionFailure()!=error || (!auto_test && status!=-EIO) || reader_exited) {
                    std::printf("Recovery precondition failed requested=%lu observed=%lu status=%d reader_exited=%d\n",
                        error,ObservedPxBcudCompletionFailure(),status,reader_exited.load());
                    switch_failed=true;break;
                }
                // 受信エラーを観測してから明示的に選局し、配信スレッドを
                // 再生成せず同じ reader へ新しい世代が渡ることを確認する。
                const auto failed_count=packets.load();
                if (!auto_test && (!receiver->SetParameters(parameters) || !receiver->Tune(5000))) {
                    std::puts("Recovery tune failed.");switch_failed=true;break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                bool resumed_lock=false;
                if (reader_exited || packets.load()<=failed_count || receiver->CheckLock(resumed_lock) || !resumed_lock ||
                    (auto_test && last_epoch.load()!=epoch_before)) {
                    std::puts("Recovery produced no data on the original reader.");switch_failed=true;break;
                }
                std::printf("Recovery passed error=%lu automatic=%d new_packets=%llu original_reader_active=1\n",
                    error,auto_test,static_cast<unsigned long long>(packets.load()-failed_count));
            }
        } else if (argc>5) {
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
        // 自動復旧の故障注入では欠落したCCを観測することが目的であり、
        // 通常受信・手動選局の「CCエラー0」という成功条件とは分けて評価する。
        const bool continuity_ok=(auto_test || limit_test || stop_test) || !cc_errors;
        const bool state_ok=stop_test ? stop_observed : limit_test ? limit_observed : locked && !lock_status;
        return packets && !sync && !tei && continuity_ok && !malformed && state_ok && !ret && !write_error && !switch_failed?0:1;
    } catch(const std::exception &ex){std::printf("Exception: %s\n",ex.what());return 1;}
}
