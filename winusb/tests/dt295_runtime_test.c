/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/DriverHost_PX4/dt295/runtime.h"
#include "mb86H/bootstrap.h"
#include "mb86H/protocol.h"
#include "mb86H/protocol/device_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
struct mock {
    uint8_t native[0x1000000],resident[MB86H_RESIDENT_BYTES],ring[MB86H_TORNE_TS_RING_BYTES];
    int torne;
    uint8_t tune[40];
    uint32_t now,generation,source;
    unsigned initialized,ready,streaming,controls,tunes,packets;
    int fail,stale;
};
static void dispatch(struct mock *m,const uint8_t request[64],uint8_t response[64])
{
    const uint32_t op=mb86H_debug_u32be_get(request+8),command=mb86H_debug_u32be_get(request+12);
    memset(response,0,64);memcpy(response,request,8);
    if(op==MB86H_DEBUG_PING){mb86H_debug_u32be_put(response+12,1);return;}
    if(op==MB86H_DEBUG_RESIDENT_BLOCK){
        const uint32_t length=mb86H_debug_u32be_get(request+20);
        CHECK(command+length<=sizeof(m->resident));CHECK(length<=40);
        if(mb86H_debug_u32be_get(request+16))memcpy(m->resident+command,request+24,length);
        else memcpy(response+24,m->resident+command,length);
        mb86H_debug_u32be_put(response+12,length);mb86H_debug_u32be_put(response+16,command);return;
    }
    CHECK(op==MB86H_DEBUG_RESIDENT_DISPATCH);
    switch(command){
    case MB86H_RESIDENT_COMMAND_CAPABILITIES:
        mb86H_debug_u32be_put(response+16,MB86H_RESIDENT_ABI_VERSION);
        memcpy(response+24,MB86H_FIRMWARE_IDENTITY,MB86H_FIRMWARE_IDENTITY_BYTES);response[39]=m->torne?1:3;
        memcpy(response+40,m->resident+MB86H_RESIDENT_BUILD_ID-MB86H_RESIDENT_BASE,8);break;
    case MB86H_RESIDENT_COMMAND_INITIALIZE:m->initialized=1;break;
    case MB86H_RESIDENT_COMMAND_SHUTDOWN:case MB86H_RESIDENT_COMMAND_RECOVER:
        m->initialized=m->ready=m->streaming=0;break;
    case MB86H_RESIDENT_COMMAND_TUNE:
        ++m->tunes;++m->generation;m->source=mb86H_debug_u32be_get(request+16);
        memcpy(m->tune,request+24,40);m->ready=1;m->streaming=m->tune[7];
        mb86H_debug_u32be_put(response+16,m->generation);break;
    case MB86H_RESIDENT_COMMAND_STOP:
        ++m->generation;m->ready=m->streaming=0;
        mb86H_debug_u32be_put(response+16,m->generation);break;
    case MB86H_RESIDENT_COMMAND_SERVICE:case MB86H_RESIDENT_COMMAND_STATUS:break;
    case MB86H_RESIDENT_COMMAND_DEVICE_QUERY:{
        unsigned page=mb86H_debug_u32be_get(request+16),source=request[25];
        uint8_t *data=response+24;data[0]=1;data[1]=(uint8_t)page;data[2]=(uint8_t)source;
        mb86H_debug_u32be_put(response+12,command);mb86H_debug_u32be_put(response+16,page);
        if(page==MB86H_DEVICE_QUERY_OVERVIEW){
            struct mb86H_device_overview d={m->torne?1u:3u,m->torne?1u:3u,MB86H_RESIDENT_ABI_VERSION,0};mb86H_query_overview_encode(data,&d);
        }else if(page==MB86H_DEVICE_QUERY_SOURCE){
            struct mb86H_source_descriptor d={.frequency_min=source?950000:473142857,
                .frequency_max=source?2150000:707143000,.frequency_step=1,.record_size=188,
                .stream_format=MB86H_STREAM_FORMAT_RAW_TS,.layers=7,
                .flags=MB86H_DEVICE_QUERY_PRESENT|MB86H_DEVICE_QUERY_STREAM|MB86H_DEVICE_QUERY_AUTO_TSID};
            mb86H_query_source_encode(data,&d);
        }else{
            struct mb86H_source_state d={.generation=m->generation-(m->stale && m->generation?1:0),
                .phase=m->generation?MB86H_RESIDENT_PHASE_COMPLETE:MB86H_RESIDENT_PHASE_QUIET,
                .flags=(m->initialized?MB86H_DEVICE_STATE_STARTED|MB86H_DEVICE_STATE_INITIALIZED:0)|
                    (m->ready?MB86H_DEVICE_STATE_READY:0)|(m->streaming?MB86H_DEVICE_STATE_CAPTURING|MB86H_DEVICE_STATE_STREAM_READY:0),
                .capture_generation=m->generation,.capture_mask=m->streaming?1u<<m->source:0};
            mb86H_query_state_encode(data,&d);
        }break;}
    default:CHECK(0);
    }
}
static int control(void *opaque,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t length,unsigned timeout)
{
    (void)timeout;struct mock *m=opaque;++m->controls;if(m->fail)return m->fail;
    CHECK(request==0xbc);uint32_t address=((uint32_t)(value&0xff00)<<8)|index;
    if((value&255)==0x7d){
        CHECK(address==(m->torne?MB86H_BOOT0_STAGING_ADDRESS:MB86H_DT295_BOOT0_STAGING_ADDRESS) && length==64);
        uint8_t response[64],request_copy[64];
        if(m->torne){CHECK(type==0xc0);memcpy(request_copy,m->native+address,64);dispatch(m,request_copy,response);memcpy(data,response,64);}
        else {CHECK(type==0x40);dispatch(m,data,response);memcpy(m->native+address,response,64);}
        return 0;
    }
    CHECK(!(value&255));uint8_t *memory=m->native+address;
    if(address>=0x80000 && address+length<=0xa0000){
        const uint32_t window=m->torne?(type==0x40?MB86H_TORNE_BOOT0_WRITE_WINDOW:MB86H_TORNE_BOOT0_READ_WINDOW):(type==0x40?MB86H_DT295_BOOT0_WRITE_WINDOW:MB86H_DT295_BOOT0_READ_WINDOW);
        const uint32_t bank=mb86H_debug_u32be_get(m->native+window);
        if(bank!=0xf00000){uint32_t physical=bank+(address&0x1ffff);
            if(physical>=MB86H_RESIDENT_BASE && physical+length<=MB86H_RESIDENT_END)memory=m->resident+physical-MB86H_RESIDENT_BASE;
            else {CHECK(m->torne && physical>=MB86H_TORNE_TS_RING_BASE && physical+length<=MB86H_TORNE_TS_RING_BASE+MB86H_TORNE_TS_RING_BYTES);memory=m->ring+physical-MB86H_TORNE_TS_RING_BASE;}
        }
    }
    if(type==0xc0)memcpy(data,memory,length);else{CHECK(type==0x40);memcpy(memory,data,length);}return 0;
}
static uint32_t now(void *ctx){return ((struct mock *)ctx)->now;}
static void delay(void *ctx,unsigned ms){((struct mock *)ctx)->now+=ms;}
static int packet(void *ctx,const uint8_t *data){CHECK(data[0]==0x47);++((struct mock *)ctx)->packets;return 0;}
int main(int argc,char **argv)
{
    CHECK(argc==2 || argc==3);const int torne=argc==3;FILE *f=fopen(argv[1],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
    uint8_t *firmware=malloc(n);CHECK(firmware && fread(firmware,1,n,f)==(size_t)n);fclose(f);
    struct mock *m=calloc(1,sizeof(*m));CHECK(m);m->torne=torne;
    struct mb86H_bootstrap b;const uint8_t *resident;
    CHECK(!mb86H_bootstrap_parse(&b,firmware,n,torne?MB86H_BOOTSTRAP_TORNE:MB86H_BOOTSTRAP_DT295,&resident));
    for(uint32_t i=0;i<b.reference_count;++i){const uint8_t *ref=b.references+i*16;
        memcpy(m->native+mb86H_debug_u32be_get(ref),firmware+mb86H_debug_u32be_get(ref+8),mb86H_debug_u32be_get(ref+4));}
    struct dt295_io io={.control=control,.now_ms=now,.delay_ms=delay,.opaque=m};struct dt295_runtime *r=NULL;
    CHECK(!dt295_create_target(&r,firmware,n,&io,torne?DT295_TARGET_TORNE:DT295_TARGET_PIX_DT295));CHECK(m->controls==0);
    CHECK(!dt295_start(r));CHECK(m->initialized);
    CHECK(!dt295_tune(r,0,539143,UINT32_MAX));CHECK(mb86H_debug_u32be_get(m->tune)==539143);CHECK(m->tune[6]==0xe0);
    CHECK(!dt295_capture(r,1));CHECK(m->streaming && m->tune[7]);
    uint8_t ts[188*4];memset(ts,0xff,sizeof(ts));for(unsigned i=0;i<4;++i)ts[i*188]=0x47;
    CHECK(!dt295_feed(r,ts,91,packet,m));CHECK(!dt295_feed(r,ts+91,sizeof(ts)-91,packet,m));CHECK(m->packets>=2);
    CHECK(!dt295_service(r));int locked=0;CHECK(!dt295_locked(r,&locked) && locked);
    m->stale=1;CHECK(!dt295_locked(r,&locked) && !locked);m->stale=0;
    CHECK(!dt295_capture(r,0));CHECK(!m->streaming);
    if(!torne){
    CHECK(!dt295_tune(r,1,1318000,0x40f1));CHECK(mb86H_debug_u32be_get(m->tune)==11996000);
    CHECK(m->tune[4]==0x40 && m->tune[5]==0xf1 && m->tune[8]==1);
    unsigned tunes=m->tunes;CHECK(dt295_tune(r,1,1318000,0x10000)==-EINVAL);CHECK(m->tunes==tunes);
    CHECK(!dt295_capture(r,1));m->packets=0;CHECK(!dt295_feed(r,ts,sizeof(ts),packet,m));CHECK(m->packets>=2);
    }else {unsigned tunes=m->tunes;CHECK(dt295_tune(r,1,1318000,0x40f1)==-EINVAL);CHECK(m->tunes==tunes);}
    CHECK(!dt295_close(r));CHECK(!m->initialized);dt295_destroy(r);
    // 保持された正確な常駐イメージへ再接続する経路も、同じ実バイナリーで検証する。
    CHECK(!dt295_create_target(&r,firmware,n,&io,torne?DT295_TARGET_TORNE:DT295_TARGET_PIX_DT295));CHECK(!dt295_start(r));CHECK(m->initialized);
    CHECK(!dt295_close(r));dt295_destroy(r);free(m);free(firmware);
    puts(torne?"TORNE real bundle + mock USB: initialize/GR/capture/TS/stale generation/restore/reopen passed":"DT295 real bundle + mock USB: initialize/GR/BS/capture/TS/stale generation/close/reopen passed");return 0;
}
