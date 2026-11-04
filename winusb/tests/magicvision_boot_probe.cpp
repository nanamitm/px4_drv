// SPDX-License-Identifier: GPL-2.0-only
// 既定は読み取り専用。明示した復元モードだけは既知の読取りウィンドウを復元する。
// boot照合を無効化する機能、reset、RF操作は持たない。
#include "../src/DriverHost_PX4/dt295/winusb_transport.hpp"
#include "msg.h"
#include <array>
#include <cstdio>

int wmain(int argc,wchar_t **argv)
{
    const bool restore_window=argc==4 && !wcscmp(argv[3],L"--restore-read-window");
    if(argc!=3 && !restore_window){puts("Usage: magicvision_boot_probe.exe <exact torne interface path> <boot dump file> [--restore-read-window]");return 2;}
    msg_set_mode(MSG_MODE_CONSOLE);
    HANDLE file=CreateFileW(argv[1],GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(file==INVALID_HANDLE_VALUE){printf("open error=%lu\n",GetLastError());return 3;}
    WINUSB_INTERFACE_HANDLE usb=nullptr;
    if(!WinUsb_Initialize(file,&usb)){CloseHandle(file);return 3;}
    USB_DEVICE_DESCRIPTOR descriptor{};ULONG actual=0;
    bool good=WinUsb_GetDescriptor(usb,USB_DEVICE_DESCRIPTOR_TYPE,0,0,reinterpret_cast<PUCHAR>(&descriptor),sizeof(descriptor),&actual) &&
        actual==sizeof(descriptor) && descriptor.idVendor==0x054c && descriptor.idProduct==0x04b2;
    HANDLE removed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!removed)good=false;
    if(good){
        px4::dt295::Transport transport(file,usb,removed);transport.Begin(15000);
        auto io=transport.Callbacks();std::array<uint8_t,0x1f20> boot{};
        auto read=[&](uint32_t address,void *data,uint16_t size){return io.control(io.opaque,0xc0,0xbc,
            static_cast<uint16_t>((address>>8)&0xff00),static_cast<uint16_t>(address),data,size,1000);};
        for(size_t offset=0;good && offset<boot.size();offset+=64){
            const auto size=static_cast<uint16_t>((boot.size()-offset)<64?boot.size()-offset:64);
            good=read(0xe9000+static_cast<uint32_t>(offset),boot.data()+offset,size)==0;
        }
        if(good){FILE *output=nullptr;good=!_wfopen_s(&output,argv[2],L"wb") && output;
            if(output){good=fwrite(boot.data(),1,boot.size(),output)==boot.size() && good;good=fclose(output)==0 && good;}}
        for(uint32_t address:{0x82008u,0xfd0090u}){
            uint8_t data[4]{};if(read(address,data,4)){good=false;break;}
            printf("read address=%08x data=%02x%02x%02x%02x\n",address,data[0],data[1],data[2],data[3]);
        }
        if(good && restore_window){
            const uint8_t hook[8]={0x9b,0x80,0,0,0xaa,4,0xe4,0x0f};
            const uint8_t write_hook[8]={0x9b,0x80,0,0,0xaa,4,0xe4,0x0d};
            const uint8_t original_out[4]={0xb4,0x27,0x9f,0x8d};
            uint8_t *window=boot.data()+0xad8;
            const uint32_t bank=uint32_t(window[0])<<24|uint32_t(window[1])<<16|uint32_t(window[2])<<8|window[3];
            good=!memcmp(boot.data()+0xac0,hook,8) && !memcmp(boot.data()+0xe0c,write_hook,8) &&
                !memcmp(boot.data()+0xd94,original_out,4) &&
                (bank==0x80380000 || bank==0x803a0000 || bank==0x82000000 || bank==0x82020000 || bank==0x82040000);
            if(good){
                uint8_t native[4]={0,0xf0,0,0};
                good=io.control(io.opaque,0x40,0xbc,0x0e00,0x9ad8,native,4,1000)==0 &&
                    read(0xe9ad8,window,4)==0 && !memcmp(window,native,4);
            }
            puts(good?"PASS restored only the owned resident read-window bank to 00f00000":"FAIL guarded window restore");
        }else puts(good?"PASS read-only boot dump; no vendor OUT or reset":"FAIL read-only boot dump");
    }
    if(removed)CloseHandle(removed);WinUsb_Free(usb);CloseHandle(file);return good?0:1;
}
