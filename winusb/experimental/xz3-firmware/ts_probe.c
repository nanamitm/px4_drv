/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#define REG(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define REG16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define RING 0x80a90000u
#define RING_SIZE 0x3fe00u
#define RUN 0x00d02010u
#define PRODUCER 0x00d02028u
#define LIMIT 0x00d0202cu
#define MAGIC 0x585a4931u

/* XZ常駐部で確認した地デジ用の入力設定。DMAの始動は最後に行う。 */
static const struct reg_setting {
    uint32_t address, value;
} setup[] = {
    {0xd00004, 0x43305},
    {0xd01040, 0}, {0xd01064, 0}, {0xd010a0, 0}, {0xd010a4, 0},
    {0xd010a8, 0}, {0xd01020, 0x02000000}, {0xd01004, 0x04000000},
    {0xd01044, 0x00bc0000}, {0xd01048, 0x47000000}, {0xd0104c, 0x00ffffff},
    {0xd01050, 0}, {0xd01060, 0}, {0xd01080, 0x01000000},
    {0xd01088, 0}, {0xd0108c, 0xffffffff}, {0xd01084, 0}, {0xd01090, 0x01000000},
    {0xd02020, RING & 0x7fffffffu}, {0xd02024, RING_SIZE}, {0xd02000, 0x03020300},
    {PRODUCER, 0}, {LIMIT, 0}, {0xd02030, 0},
};
#define NSETUP (sizeof(setup)/sizeof(setup[0]))

#define SELECTOR 0xfd814cu
#define EP_MODE 0xfdbc04u
#define EP_CONTROL 0xfdbc14u
#define EP_BUFFER 0xfdbd08u
#define EP_STATUS 0xfdbd0cu
#define EP_FIFO 0xfdbd84u
#define BULK_SIZE 196608u
#define STREAM_UNIT (3u * 1024 * 1024)

/* 本体と一緒に初期化し、STARTからSTOPまでの退避値を保持する。 */
static struct {
    uint32_t saved[NSETUP];
    uint16_t pin, pin_next;
    unsigned cursor;
    unsigned active;
} capture __attribute__((section(".probe_state")));

static void ep_write(unsigned group,unsigned address,uint32_t value)
{
    REG(SELECTOR)=group;
    REG(address)=value;
    REG(SELECTOR)=15;
}

static unsigned ep_ready(void)
{
    for(unsigned polls=0;polls<250000;++polls)
        if(REG(EP_STATUS)&4) return 1;
    return 0;
}

static void bulk_snapshot(uint32_t *reply)
{
    reply[4]=REG(EP_MODE);
    reply[5]=REG(EP_CONTROL);
    reply[6]=REG(EP_BUFFER);
    /* USB列挙時のEP81設定を使い、送信直後にFIFOをリセットしない。 */
    if(REG(SELECTOR)!=15 || reply[4]!=0xd7000000 ||
       reply[5]!=0x00040032 || reply[6]!=0x00ff6000) return;
    unsigned sent=0;
    while(sent<BULK_SIZE && ep_ready()) {
        for(unsigned i=0;i<512;i+=4)
            REG(EP_FIFO)=REG(RING+sent+i);
        ep_write(8,EP_STATUS,4);
        sent+=512;
    }
    reply[7]=sent;
    reply[8]=ep_ready();
    reply[9]=REG(EP_STATUS);
}
static void stream_ring(uint32_t *reply,unsigned target)
{
    unsigned cursor=capture.cursor, sent=0, peak=0, producer=0;
    /* リングとUSBパケットはいずれも512バイト境界で周回する。 */
    while(sent<target) {
        unsigned available=0;
        for(unsigned polls=0;polls<(sent ? 250000u : 8000000u);++polls) {
            producer=REG(PRODUCER);
            if(producer>=RING_SIZE) break;
            available=producer>=cursor ? producer-cursor : RING_SIZE-cursor+producer;
            if(available>=512 && (REG(EP_STATUS)&4)) break;
        }
        if(producer>=RING_SIZE || available<512 || !(REG(EP_STATUS)&4)) break;
        if(available>peak) peak=available;
        for(unsigned i=0;i<512;i+=4)
            REG(EP_FIFO)=REG(RING+cursor+i);
        ep_write(8,EP_STATUS,4);
        cursor+=512;
        if(cursor==RING_SIZE) cursor=0;
        REG(LIMIT)=cursor;
        sent+=512;
    }
    capture.cursor=cursor;
    reply[4]=peak;
    reply[5]=cursor;
    reply[6]=producer;
    reply[7]=sent;
    reply[8]=ep_ready();
    reply[9]=REG(EP_STATUS);
}
static unsigned endpoint_ready(void)
{
    return REG(SELECTOR)==15 && REG(EP_MODE)==0xd7000000 &&
           REG(EP_CONTROL)==0x00040032 && REG(EP_BUFFER)==0x00ff6000;
}

static void start_capture(unsigned reset)
{
    capture.cursor=0;
    for(unsigned i=0;i<NSETUP;++i) capture.saved[i]=REG(setup[i].address);
    capture.pin=REG16(0xd01000);
    capture.pin_next=REG16(0xd01002);
    if(reset && REG(0xd00000)) {
        REG(0xd00004)=0x001f33ff;
        REG(0xd00000)=0x30000fff;
        REG(0xd00000)=0;
        REG(0xd00004)=0x3305;
    }
    REG16(0xd01000)=0x600;
    REG16(0xd01002)=0;
    for(unsigned i=0;i<NSETUP;++i) REG(setup[i].address)=setup[i].value;
    REG16(0xd01000)=0x607;
    REG(0xd01020)=0;
    REG(0xd01040)=0x03000000;
    REG(0xd010a4)=0x13170400;
    REG(RUN)=0x02000000;
    capture.active=1;
}

static uint32_t stop_capture(void)
{
    uint32_t state=0;
    /* リングを読み出す前に入力とDMAを停止する。 */
    REG(0xd01040)=0;
    REG(0xd01020)=0x02000000;
    REG(RUN)=0x04000000;
    for(unsigned i=0;i<64000;++i) {
        state=REG(RUN);
        if(!state) break;
    }
    return state;
}

static void restore_capture(uint32_t state)
{
    if(state) return;
    for(unsigned i=NSETUP;i>0;--i) REG(setup[i-1].address)=capture.saved[i-1];
    REG16(0xd01000)=capture.pin;
    REG16(0xd01002)=capture.pin_next;
    capture.active=0;
}

void xz_i2c_relay(uint32_t packet[16])
{
    const unsigned op=packet[2];
    if(packet[0]!=MAGIC || op<8 || op>13 || packet[3] || packet[4] || packet[5])
        return;
    const unsigned stream=op==10;
    const unsigned bulk=op==9;
    const unsigned stream_target=STREAM_UNIT;
    uint32_t *reply=packet+6;
    for(unsigned i=0;i<10;++i) reply[i]=0;
    packet[2]|=0x80000000u;
    packet[3]=6;
    packet[5]=40;
    reply[0]=REG(RUN);
    if(op==13) {
        if(capture.active) {
            reply[3]=stop_capture();
            restore_capture(reply[3]);
            packet[3]=reply[3] ? 2 : 0;
        }
        return;
    }
    if(op==12) {
        if(capture.active && endpoint_ready()) {
            stream_ring(reply,stream_target);
            reply[1]=REG(RUN);
            reply[2]=REG(PRODUCER);
            packet[3]=reply[7]==stream_target ? 0 : 2;
        }
        return;
    }
    if(capture.active || reply[0] || REG(0xd03010) || REG16(0xf10114)!=0x8008 ||
       REG16(0xf10120)!=0x100 || REG16(0xf1010c)!=0x8201 ||
       ((stream || op==11) && !endpoint_ready()))
        return;
    if(op==11) {
        /* 以前の要求の末尾を次のEP81読み出しへ渡さない。 */
        ep_write(8,EP_BUFFER,1);
        ep_write(6,EP_BUFFER,0x00ff6000);
        REG(EP_BUFFER)=REG(EP_BUFFER);
    }
    start_capture(stream || op==11);
    if(op==11) {
        packet[3]=0;
        return;
    }
    if(stream) stream_ring(reply,stream_target);
    const unsigned target=bulk ? BULK_SIZE : 576;
    uint32_t producer=0;
    for(unsigned i=0;!stream && i<(bulk ? 8000000u : 1000000u);++i) {
        producer=REG(PRODUCER);
        if(producer>=target) break;
    }
    reply[1]=REG(RUN);
    reply[2]=stream ? REG(PRODUCER) : producer;
    reply[3]=stop_capture();
    if(stream) {
        packet[3]=reply[7]==stream_target && reply[8] && !reply[3] ? 0 : 2;
    } else if(producer>=target && producer<RING_SIZE && !reply[3]) {
        for(unsigned i=0;i<3;++i) {
            reply[4+2*i]=REG(RING+192*i);
            reply[5+2*i]=REG(RING+192*i+4);
        }
        packet[3]=0;
    } else {
        packet[3]=2;
    }
    if(bulk && !packet[3]) {
        bulk_snapshot(reply);
        if(reply[7]!=BULK_SIZE || !reply[8]) packet[3]=2;
    }
    restore_capture(reply[3]);
}
