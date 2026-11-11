// SPDX-License-Identifier: GPL-2.0-only
#include <windows.h>
#include "IBonDriver2.h"
#include "../src/common/ts_continuity.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

// 明示したBonDriverだけを開き、選局・TS保存・同一プロセスの再オープンを測定する実機用ツール。
int wmain(int argc,wchar_t **argv)
{
    if(argc!=7){puts("Usage: resident_capture.exe <BonDriver.dll> <space> <channel> <measure-ms> <output-base> <reopens>");return 2;}
    const DWORD space=wcstoul(argv[2],nullptr,10),channel=wcstoul(argv[3],nullptr,10);
    const unsigned ms=wcstoul(argv[4],nullptr,10),runs=wcstoul(argv[6],nullptr,10);
    if(!ms || ms>3600000 || !runs || runs>20)return 2;
    HMODULE module=LoadLibraryW(argv[1]);if(!module)return 3;
    auto create=reinterpret_cast<IBonDriver *(*)()>(GetProcAddress(module,"CreateBonDriver"));
    IBonDriver2 *driver=create?static_cast<IBonDriver2 *>(create()):nullptr;
    if(!driver){FreeLibrary(module);return 3;}
    bool good=true;
    for(unsigned run=0;run<runs && good;++run){
        auto before=std::chrono::steady_clock::now();
        if(!driver->OpenTuner()){printf("open failed run=%u\n",run);good=false;break;}
        if(!driver->SetChannel(space,channel)){printf("tune failed run=%u\n",run);driver->CloseTuner();good=false;break;}
        const auto tune_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before).count();
        driver->PurgeTsStream();
        const std::wstring path=std::wstring(argv[5])+L".run"+std::to_wstring(run)+L".ts";
        FILE *out=nullptr;if(_wfopen_s(&out,path.c_str(),L"wb") || !out){driver->CloseTuner();good=false;break;}
        auto monitor=std::make_unique<px4::TsContinuityMonitor>();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
        unsigned long long bytes=0;unsigned malformed_buffers=0;
        while(good && std::chrono::steady_clock::now()<deadline){
            driver->WaitTsStream(100);
            while(driver->GetReadyCount() && std::chrono::steady_clock::now()<deadline){
                BYTE *data=nullptr;DWORD size=0,remain=0;
                if(!driver->GetTsStream(&data,&size,&remain) || !data)break;
                if(size%188)++malformed_buffers;
                monitor->Feed(data,size,[](const px4::TsContinuityMonitor::Event &){});
                if(fwrite(data,1,size,out)!=size){good=false;break;}bytes+=size;
            }
        }
        if(fclose(out))good=false;
        auto closing=std::chrono::steady_clock::now();driver->CloseTuner();
        const auto close_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-closing).count();
        const auto &c=monitor->Counts();
        printf("run=%u tune_ms=%lld close_ms=%lld bytes=%llu packets=%llu sync=%llu tei=%llu cc_jump=%llu same_cc_changed=%llu malformed=%llu malformed_buffers=%u\n",
            run,static_cast<long long>(tune_ms),static_cast<long long>(close_ms),bytes,
            static_cast<unsigned long long>(c.packets),static_cast<unsigned long long>(c.sync),
            static_cast<unsigned long long>(c.tei),static_cast<unsigned long long>(c.cc_jump),
            static_cast<unsigned long long>(c.repeat_changed),static_cast<unsigned long long>(c.malformed),malformed_buffers);
        fflush(stdout);
        good=good && c.packets && !c.sync && !c.tei && !c.cc_jump && !c.repeat_changed && !c.malformed && !malformed_buffers;
    }
    driver->Release();FreeLibrary(module);return good?0:1;
}
