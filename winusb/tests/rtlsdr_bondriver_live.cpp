// SPDX-License-Identifier: GPL-2.0-only
#include <windows.h>
#include "../include/IBonDriver2.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>

void Require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
std::uint32_t Crc(const BYTE *data,std::size_t size)
{
    std::uint32_t value=0xffffffff;
    for(std::size_t i=0;i<size;++i){value^=std::uint32_t(data[i])<<24;for(unsigned bit=0;bit<8;++bit)value=(value<<1)^((value&0x80000000)?0x04c11db7:0);}
    return value;
}
int wmain(int argc,wchar_t **argv)
{
    if(argc!=5 && argc!=6){std::fprintf(stderr,"usage: rtlsdr_bondriver_live DRIVER OUTPUT_PREFIX SECONDS CYCLES [ALTERNATE_CHANNEL]\n");return 2;}
    HMODULE module=nullptr;IBonDriver *base=nullptr;
    HANDLE file=INVALID_HANDLE_VALUE;
    try{
        const auto seconds=std::stoul(argv[3]),cycles=std::stoul(argv[4]);
        const auto alternate=argc==6?std::stoul(argv[5]):0;
        Require(seconds>=1&&seconds<=600&&cycles>=1&&cycles<=10,"Invalid duration/cycles");
        Require(argc!=6 || (alternate>=1 && alternate<=39 && cycles>=2),"Invalid alternate channel/cycles");
        module=LoadLibraryW(argv[1]);Require(module!=nullptr,"Cannot load BonDriver");
        using Factory=IBonDriver *(*)();
#pragma warning(suppress:4191)
        const auto factory=reinterpret_cast<Factory>(GetProcAddress(module,"CreateBonDriver"));Require(factory!=nullptr,"Missing CreateBonDriver");
        base=factory();Require(base!=nullptr,"CreateBonDriver failed");
        auto *driver=dynamic_cast<IBonDriver2 *>(base);Require(driver!=nullptr,"IBonDriver2 RTTI failed");
        std::array<int,2> expected_tsid={-1,-1};
        for(unsigned long cycle=1;cycle<=cycles;++cycle){
            const auto slot=argc==6?(cycle-1)%2:0;
            const auto channel=slot?alternate:0;
            if(argc==5 || cycle==1)Require(driver->OpenTuner()!=FALSE,"OpenTuner failed");
            const auto tune_start=GetTickCount64();
            Require(driver->SetChannel(0,channel)!=FALSE,"SetChannel failed");
            const auto tune_ms=GetTickCount64()-tune_start;
            if(GetEnvironmentVariableW(L"RTLSDR_EXPECT_ASYNC_TUNE",nullptr,0)){
                Require(tune_ms<500,"Async tune response exceeded 500 ms");
                Require(driver->GetSignalLevel()==0,"Async request acceptance falsely reported RF lock");
            }
            Require(driver->GetCurSpace()==0&&driver->GetCurChannel()==channel,"Current channel mismatch");
            const std::wstring output=std::wstring(argv[2])+L".cycle"+std::to_wstring(cycle)+L".ts";
            file=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            Require(file!=INVALID_HANDLE_VALUE,"Cannot create capture (already exists?)");
            std::array<int,8192> last;last.fill(-1);
            std::uint64_t packets=0,sync=0,tei=0,cc=0,pat=0,bad_pat=0,pmt=0,empty=0;
            ULONGLONG last_data=GetTickCount64();
            ULONGLONG begin=0,first_ts_ms=0;
            const auto startup=GetTickCount64();
            while(!begin || GetTickCount64()-begin<seconds*1000){
                BYTE *data=nullptr;DWORD size=0,remain=0;
                // BonDriver_PX4 は読取可能バッファがない場合にも FALSE を返す。
                if(driver->GetTsStream(&data,&size,&remain)==FALSE)size=0;
                if(!size){
                    // 非同期選局は要求受付だけを返す。初回TSは旧TuneTimeoutと同じ20秒以内を要求し、
                    // 受信開始後の3秒超の途切れとは別に検証する。
                    if(!begin)Require(GetTickCount64()-startup<20000,"Initial TS did not arrive within 20 seconds");
                    else if(GetTickCount64()-last_data>3000){++empty;last_data=GetTickCount64();}
                    Sleep(5);continue;
                }
                if(!begin){
                    Require(driver->GetSignalLevel()>0,"Old queued TS arrived before the new receiver locked");
                    begin=GetTickCount64();first_ts_ms=begin-tune_start;
                }
                Require(data && size%188==0,"Unaligned BonDriver TS");last_data=GetTickCount64();
                DWORD written=0;Require(WriteFile(file,data,size,&written,nullptr)&&written==size,"TS file write failed");
                for(DWORD pos=0;pos<size;pos+=188){
                    const auto *p=data+pos;++packets;if(p[0]!=0x47){++sync;continue;}
                    if(p[1]&0x80)++tei;
                    const auto pid=((p[1]&31)<<8)|p[2];const auto afc=(p[3]>>4)&3;
                    Require(afc && (!(afc&2)||p[4]<=183),"Malformed adaptation field");
                    if((afc&2)&&p[4]>=1&&(p[5]&0x80))last[pid]=-1;
                    if(pid!=8191&&(afc&1)){const int value=p[3]&15;if(last[pid]>=0&&value!=last[pid]&&value!=((last[pid]+1)&15))++cc;last[pid]=value;}
                    if(pid==0x1fc8)++pmt;
                    if(pid==0&&(p[1]&0x40)&&(afc&1)){
                        std::size_t offset=(afc&2)?5+p[4]:4;
                        if(offset>=188){++bad_pat;continue;}
                        offset+=1+p[offset];
                        if(offset+3>188){++bad_pat;continue;}
                        const std::size_t length=3+((p[offset+1]&15)<<8)+p[offset+2];
                        if(p[offset]!=0||length<12||length>188-offset||Crc(p+offset,length)!=0)++bad_pat;else {
                            ++pat;
                            if(argc==6){
                                const int tsid=(p[offset+3]<<8)|p[offset+4];
                                // 同じPIDを使うワンセグ局もあるため、PATのTSIDで旧局の混入を検出する。
                                if(expected_tsid[slot]<0)expected_tsid[slot]=tsid;
                                Require(tsid==expected_tsid[slot],"TSID changed within channel");
                                Require(tsid!=expected_tsid[slot^1],"Previous channel PAT leaked / channels need distinct TSIDs");
                            }
                        }
                    }
                }
            }
            CloseHandle(file);file=INVALID_HANDLE_VALUE;
            std::printf("cycle=%lu tune_ms=%llu first_ts_ms=%llu packets=%llu sync=%llu tei=%llu cc_gaps=%llu pat=%llu invalid_pat=%llu pmt=%llu empty_intervals=%llu quality=%.2f\n",
                cycle,tune_ms,first_ts_ms,packets,sync,tei,cc,pat,bad_pat,pmt,empty,driver->GetSignalLevel());std::fflush(stdout);
            Require(packets>0&&!sync&&!tei&&!cc&&pat>0&&!bad_pat&&pmt>0&&!empty,"TS/PAT/continuity validation failed");
            // 同じ接続で再選局し、その後 Close/Open も繰り返して helper の寿命を検証する。
            if(argc==5){
                Require(driver->SetChannel(0,0)!=FALSE,"Retune failed");driver->PurgeTsStream();
                driver->CloseTuner();
            }
        }
        if(argc==6)driver->CloseTuner();
        base->Release();base=nullptr;FreeLibrary(module);module=nullptr;return 0;
    }catch(const std::exception &error){
        std::fprintf(stderr,"RTL-SDR live test: %s\n",error.what());
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(base){base->CloseTuner();base->Release();}if(module)FreeLibrary(module);return 1;
    }
}
