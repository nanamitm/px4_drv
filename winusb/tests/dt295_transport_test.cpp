// SPDX-License-Identifier: GPL-2.0-only
#include "../src/DriverHost_PX4/dt295/winusb_transport.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cerrno>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);std::exit(1);}}while(0)
extern "C" int msg_printf(const char *,...){return 0;}
using namespace px4::dt295;
struct Mock final : ControlIo {
    DWORD submit=ERROR_IO_PENDING,collect=0;
    bool signal=true,short_read=false,remove=false;
    unsigned submits=0,collects=0,cancels=0;
    HANDLE removed=nullptr;PUCHAR buffer=nullptr;ULONG length=0;
    WINUSB_SETUP_PACKET setup{};
    DWORD Submit(WINUSB_INTERFACE_HANDLE,WINUSB_SETUP_PACKET packet,PUCHAR bytes,ULONG size,ULONG &actual,OVERLAPPED &op) override {
        ++submits;setup=packet;buffer=bytes;length=size;actual=short_read?size-1:size;
        if(signal)SetEvent(op.hEvent);
        if(remove)SetEvent(removed);
        return submit;
    }
    DWORD Collect(WINUSB_INTERFACE_HANDLE,OVERLAPPED &op,ULONG &actual) override {
        ++collects;CHECK(WaitForSingleObject(op.hEvent,0)!=WAIT_FAILED);
        actual=short_read?length-1:length;
        if(length)buffer[0]=0x5a;return collect;
    }
    void Cancel(HANDLE,OVERLAPPED &) noexcept override {++cancels;}
};
int main(int argc,char **argv){
    CHECK(argc==2);FILE *f=nullptr;CHECK(!fopen_s(&f,argv[1],"rb") && f);
    CHECK(!fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>64);rewind(f);
    std::vector<uint8_t> firmware(static_cast<size_t>(size));CHECK(fread(firmware.data(),1,firmware.size(),f)==firmware.size());fclose(f);
    CHECK(dt295_validate(firmware.data(),firmware.size())==0);
    CHECK(dt295_validate(firmware.data(),63)<0);
    auto broken=firmware;broken[0]^=1;CHECK(dt295_validate(broken.data(),broken.size())<0);
    broken=firmware;broken.back()^=1; // immutable領域の破損は別途先頭のイメージ位置から注入する。
    const size_t image=(size_t(firmware[40])<<24)|(size_t(firmware[41])<<16)|(size_t(firmware[42])<<8)|firmware[43];
    CHECK(image+262144<=firmware.size());broken[image+64]^=1;CHECK(dt295_validate(broken.data(),broken.size())<0);
    HANDLE removed=CreateEventW(nullptr,TRUE,FALSE,nullptr);CHECK(removed);
    uint8_t bytes[64]{};
    for(unsigned test=0;test<8;++test){
        ResetEvent(removed);Mock io;io.removed=removed;
        if(test==1)io.short_read=true;
        if(test==2){io.signal=false;io.collect=ERROR_OPERATION_ABORTED;}
        if(test==3){io.remove=true;io.collect=ERROR_OPERATION_ABORTED;}
        if(test==4)io.submit=ERROR_DEVICE_NOT_CONNECTED;
        if(test==5)io.submit=ERROR_SUCCESS;
        Transport transport(INVALID_HANDLE_VALUE,nullptr,removed,&io);transport.Begin(1000);
        if(test==6)SetEvent(removed);
        if(test==7)transport.Begin(0);
        auto callbacks=transport.Callbacks();
        int rc=callbacks.control(callbacks.opaque,0x40,0xbc,0x0c7d,0x4620,bytes,64,2);
        if(test==0 || test==5){CHECK(rc==0);CHECK(io.setup.Value==0x0c7d && io.setup.Index==0x4620 && io.setup.Length==64);}
        else {
            CHECK(rc<0);if(test==1)CHECK(rc==-EPROTO);if(test==2 || test==7)CHECK(rc==-ETIMEDOUT);
            if(test==3 || test==4 || test==6)CHECK(rc==-ENODEV);
            unsigned calls=io.submits;transport.Begin(1000);
            CHECK(callbacks.control(callbacks.opaque,0x40,0xbc,0,0,bytes,64,2)==rc);CHECK(io.submits==calls);
        }
        CHECK(io.collects==((test<=3)?1u:0u));CHECK(io.cancels==((test==2 || test==3)?1u:0u));
        dt295_runtime *runtime=nullptr;
        CHECK(dt295_create(&runtime,firmware.data(),firmware.size(),&callbacks)==0);
        CHECK(dt295_tune(runtime,0,539143,UINT32_MAX)<0); // start前の送信を拒否する。
        CHECK(dt295_close(runtime)==0);dt295_destroy(runtime);
    }
    ResetEvent(removed);
    {
        static const uint8_t immutable[64]={0x31};Mock io;
        Transport transport(INVALID_HANDLE_VALUE,nullptr,removed,&io);transport.Begin(1000);auto callbacks=transport.Callbacks();
        CHECK(!callbacks.control(callbacks.opaque,0x40,0xbc,0,0,const_cast<uint8_t *>(immutable),64,100));
        CHECK(io.buffer!=immutable && immutable[0]==0x31 && io.collects==1);
    }
    CloseHandle(removed);puts("DT295 firmware, writable OUT copy, cancellation, short transfer, disconnect, no-replay tests passed");return 0;
}
