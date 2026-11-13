/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#ifdef XZ_TS_RUNTIME
#include "i2c_transfer.h"
#endif
#define REG(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define REG16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define RING 0x80a90000u
/* mAgicVISIONの0x3fe00ではCSの約39 ms分しかない。torneと同じ6 MiBにする。
   0x80000000〜0x83ffffffは折り返さず、0x80b8fe00〜0x8108ffffは受信中に書き込まれないことを
   memory_probe.cで確認した。DriverHostのxz3/device.cppのRingBytesと一致させる。 */
#define RING_SIZE 0x600000u
#define RUN 0x00d02010u
#define PRODUCER 0x00d02028u
#define LIMIT 0x00d0202cu
#define MAGIC 0x585a4931u

/*
 * XZ常駐部で確認した地デジ用の入力設定。DMAの始動は最後に行う。
 * 番地はすべてTSブロック (0xd00000〜0xd03fff) にあるため、本体の窓を節約するよう
 * 0xd00000からの16ビットの差分と値を別の表に持つ。書き込む値と順序は同じ。
 */
#define SETUP_BASE 0xd00000u
static const uint16_t setup_offset[] = {
    0x0004,
    0x1040, 0x1064, 0x10a0, 0x10a4,
    0x10a8, 0x1020, 0x1004,
    0x1044, 0x1048, 0x104c,
    0x1050, 0x1060, 0x1080,
    0x1088, 0x108c, 0x1084, 0x1090,
    0x2020, 0x2024, 0x2000,
    PRODUCER - SETUP_BASE, LIMIT - SETUP_BASE, 0x2030,
};
static const uint32_t setup_value[] = {
    0x43305,
    0, 0, 0, 0,
    0, 0x02000000, 0x04000000,
    0x00bc0000, 0x47000000, 0x00ffffff,
    0, 0, 0x01000000,
    0, 0xffffffff, 0, 0x01000000,
    RING & 0x7fffffffu, RING_SIZE, 0x03020300,
    0, 0, 0,
};
#define NSETUP (sizeof(setup_value)/sizeof(setup_value[0]))
_Static_assert(sizeof(setup_offset)/sizeof(setup_offset[0]) == NSETUP, "setup tables must match");
#define SETUP_REG(i) REG(SETUP_BASE + setup_offset[i])

#define SELECTOR 0xfd814cu
#define EP_MODE 0xfdbc04u
#define EP_CONTROL 0xfdbc14u
#define EP_BUFFER 0xfdbd08u
#define EP_STATUS 0xfdbd0cu
#define EP_FIFO 0xfdbd84u
#define BULK_SIZE 196608u
/* 操作12の既定かつ最大の送出量。 */
#define STREAM_UNIT (3u * 1024 * 1024)
/*
 * 操作12の送出量の単位。192バイトのレコードと512バイトのUSB送出の最小公倍数で、
 * 6 MiBのリングも割り切れるため、各要求はレコード境界で終わる。
 */
#define STREAM_GRAIN 1536u

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

#ifndef XZ_TS_RUNTIME
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
#endif

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
#ifndef XZ_TS_RUNTIME
    /* 診断版だけが使う。共通版は本体の窓を節約するため返さない。 */
    reply[8]=ep_ready();
    reply[9]=REG(EP_STATUS);
#endif
}
static unsigned endpoint_ready(void)
{
    return REG(SELECTOR)==15 && REG(EP_MODE)==0xd7000000 &&
           REG(EP_CONTROL)==0x00040032 && REG(EP_BUFFER)==0x00ff6000;
}

static void start_capture(unsigned reset)
{
    capture.cursor=0;
    for(unsigned i=0;i<NSETUP;++i) capture.saved[i]=SETUP_REG(i);
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
    for(unsigned i=0;i<NSETUP;++i) SETUP_REG(i)=setup_value[i];
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
    for(unsigned i=NSETUP;i>0;--i) SETUP_REG(i-1)=capture.saved[i-1];
    REG16(0xd01000)=capture.pin;
    REG16(0xd01002)=capture.pin_next;
    capture.active=0;
}

#ifdef XZ_TS_RUNTIME
static uint32_t read_cn(unsigned address)
{
    /* 地デジは0x18の0x8bから3バイト、衛星は0x19の0xbcから2バイト。0x18以外は衛星として扱う。 */
    const unsigned terrestrial = address == 0x18;
    const uint8_t reg = terrestrial ? 0x8b : 0xbc;
    const unsigned size = terrestrial ? 3 : 2;
    uint8_t bytes[3];
    /* DMAの消費を止める時間を抑えるため、統計取得の待機上限を短くする。 */
    if (transfer_with_budget(terrestrial ? 0x18 : 0x19, &reg, 1, bytes, size, 4000u))
        return 0;
    uint32_t raw = 0;
    for (unsigned i = 0; i < size; ++i)
        raw = (raw << 8) | bytes[i];
    return raw;
}
static void tune_transfer(uint32_t packet[16])
{
    const unsigned tx = packet[3], address = packet[4], rx = packet[5];
    unsigned error = XZ_INVALID;
    uint8_t *payload = (uint8_t *)(packet + 6);
    /* 選局操作はDMAを止めてから。送信を終えたペイロードへ受信データを格納する。 */
    /* 長さ0の転送は復調器のアドレスだけを送り、状態を変えないため拒否しない。 */
    if (!capture.active && (address == 0x18 || address == 0x19) &&
        tx <= 40 && rx <= 40 - tx) {
        error = XZ_NOT_READY;
        if ((XZ_I2C_READ(I2C_DIVIDER) & 0x7fu) == 0x3cu)
            error = transfer(address, payload, tx, payload, rx);
    }
    packet[2] |= 0x80000000u;
    packet[3] = error;
    packet[4] = error ? 0 : 1;
    packet[5] = error ? 0 : rx;
}
/*
 * 操作14: カードUARTのFIFOの一括転送。boot0のメモリ要求は1語ごとに約3 msかかり、
 * カードの1バイトごとに1往復では、受信中のT=1の応答待ちに間に合わない。
 * ワード3のバイト数をペイロードから送信FIFO (0xfea028) へ積み、続けて受信FIFO
 * (0xfea014の上位16ビットが受信数) から最大31バイトをペイロードへ読む。
 * 応答のワード5は読んだバイト数。送信開始などのレジスター操作はホストが行う。
 * 本体の窓に余裕がないため、範囲の検査の代わりに送信のバイト数を下位5ビット
 * (0〜31) へ切り詰め、どちらも40バイトのペイロードを超えないようにする。
 * DMAの動作中も実行でき、TSのリングとI²Cには触れない。
 */
static void card_fifo(uint32_t packet[16])
{
    volatile uint32_t *uart = (volatile uint32_t *)0xfea000u;
    const unsigned tx = packet[3] & 31u;
    uint8_t *payload = (uint8_t *)(packet + 6);
    for (unsigned i = 0; i < tx; ++i)
        uart[10] = payload[i];
    unsigned count = uart[5] >> 16;
    if (count > 31u)
        count = 31u;
    for (unsigned i = 0; i < count; ++i)
        payload[i] = (uint8_t)uart[11];
    packet[2] |= 0x80000000u;
    packet[3] = 0;
    packet[5] = count;
}
#endif

void xz_i2c_relay(uint32_t packet[16])
{
    const unsigned op=packet[2];
#ifdef XZ_TS_RUNTIME
    if(packet[0]!=MAGIC)
        return;
    if(op==7) {
        tune_transfer(packet);
        return;
    }
    if(op==14) {
        card_fifo(packet);
        return;
    }
    const unsigned cn_address=packet[4];
    /*
     * 操作12のワード5の下位2ビットは送出量の段階で、STREAM_UNITを右へずらす数
     * （0から3で3 MiB、1.5 MiB、768 KiB、384 KiB）。どの段階もSTREAM_GRAINの倍数になる。
     * 操作12の間はEP0が応答せず、カードUARTの操作を挟めない。ホストは流量に合わせて
     * 1回の時間を短く保つ一方、要求ごとの固定の負担がUSB FIFOへの送出の余裕を削らない
     * 段階を選ぶ。本体の窓に余裕がないため、範囲の検査の代わりに2ビットへ切り詰める。
     * 操作11と13はstream_targetを使わない。
     * ワード4のC/Nの番地も検査の代わりに、read_cnが0x18以外を0x19として扱う。
     * 操作11と13はワード4を使わない。
     */
    if(op<11 || op>13 || packet[3])
        return;
    const unsigned stream=0;
    const unsigned stream_target=STREAM_UNIT>>(packet[5]&3u);
#else
    if(packet[0]!=MAGIC || op<8 || op>13 || packet[3] || packet[4] || packet[5])
        return;
    const unsigned stream=op==10;
    const unsigned bulk=op==9;
    const unsigned stream_target=STREAM_UNIT;
#endif
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
#ifndef XZ_TS_RUNTIME
            /* 共通版は本体の窓を節約するため返さない。消費前の書き込み位置はreply[6]にある。 */
            reply[1]=REG(RUN);
            reply[2]=REG(PRODUCER);
#endif
            packet[3]=reply[7]==stream_target ? 0 : 2;
#ifdef XZ_TS_RUNTIME
            /* DMAは動作したまま。I²C失敗はC/Nだけを無効にし、TSの結果に伝播させない。 */
            packet[4]=!packet[3] && cn_address ? read_cn(cn_address) : 0;
#endif
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
#ifndef XZ_TS_RUNTIME
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
#endif
}
