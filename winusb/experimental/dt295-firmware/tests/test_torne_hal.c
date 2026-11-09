/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
    uint32_t state, control, now, generation, calls, fail_at;
    uint32_t address[256], value[256], width[256], writes;
    uint8_t input[40], cursor;
    int exclusive;
};
static int access_ok(struct fake *f) { return ++f->calls != f->fail_at; }
static int read32(void *p, uint32_t a, uint32_t *v)
{
    struct fake *f = p;
    if (!access_ok(f)) return 0;
    switch (a) {
    case 0xfd8010: *v = 0x100; break;
    case 0xfd8014: *v = 0x110; break;
    case 0xfd8fe0: *v = f->state; break;
    case 0xfd8fe4: *v = f->control; break;
    case 0xfd8ff0: assert(f->cursor < 40); *v = f->input[f->cursor++]; break;
    default: assert(0);
    }
    return 1;
}
static int record(struct fake *f, uint32_t a, uint32_t v, uint32_t width)
{
    uint32_t i;
    if (!access_ok(f)) return 0;
    i = f->writes++;
    assert(i < 256);
    f->address[i] = a; f->value[i] = v; f->width[i] = width;
    if (a == 0xfd8fe4) f->control = v;
    return 1;
}
static int write32(void *p, uint32_t a, uint32_t v) { return record(p,a,v,32); }
static int write16(void *p, uint32_t a, uint16_t v) { return record(p,a,v,16); }
static uint32_t now_us(void *p) { return ((struct fake *)p)->now; }
static int exclusive(void *p, uint32_t g)
{
    struct fake *f = p;
    return f->exclusive && f->generation == g;
}
static void setup(struct fake *f, struct dtp_torne_hal *h)
{
    struct dtp_torne_bus bus = {f,read32,write32,write16,now_us,exclusive};
    memset(f,0,sizeof(*f));
    f->exclusive = 1; f->generation = 7;
    dtp_torne_init(h,&bus,7);
    assert(f->calls == 0);
}
static void prepare(struct fake *f, struct dtp_torne_hal *h)
{
    static const uint32_t a[] = {0xfd8010,0xfd8014,0xfd80c0,0xfd80c0,0xfd80c4,
        0x90114,0xfd8ff8,0xfd8ff4,0xfd8fe8,0xfd8fe4};
    static const uint32_t v[] = {0x110,0x100,1,0,0,0x8008,6,8,0x3c,0x40};
    uint32_t i;
    setup(f,h);
    assert(dtp_torne_prepare(h) == DTP_I2C_OK);
    assert(f->writes == 10);
    for (i = 0; i < 10; ++i) {
        assert(f->address[i] == a[i] && f->value[i] == v[i]);
        assert(f->width[i] == (i == 5 ? 16u : 32u));
    }
    assert(dtp_torne_prepare(h) == DTP_I2C_BUSY);
}
static enum dtp_i2c_result completed(struct fake *f, struct dtp_torne_hal *h)
{
    f->control |= 1;
    return dtp_torne_poll(h);
}
static void transfer(void)
{
    struct fake f;
    struct dtp_torne_hal h;
    uint8_t tx[] = {3,0x80}; /* DiB8096 vendor register 896 */
    uint32_t before;
    static const uint32_t commands[] = {0x50,0x50,0x50,0x70,0x58,0x50,0x40};
    uint32_t i, count = 0;
    prepare(&f,&h);
    f.input[0] = 1; f.input[1] = 0xb3;
    assert(dtp_torne_start(&h,9,tx,2,2,100) == DTP_I2C_PENDING);
    assert(f.value[10] == 0x12);
    before = f.writes;
    f.state = 0x10; /* 古いNACKは未完了時に評価しない。 */
    assert(dtp_torne_poll(&h) == DTP_I2C_PENDING && f.writes == before);
    f.state = 0;
    for (i = 0; i < 6; ++i) assert(completed(&f,&h) == DTP_I2C_PENDING);
    f.state = 0x80;
    assert(dtp_torne_poll(&h) == DTP_I2C_PENDING);
    f.state = 0;
    assert(dtp_torne_poll(&h) == DTP_I2C_OK);
    assert(h.engine.rx_completed == 2 && h.engine.rx[0] == 1 && h.engine.rx[1] == 0xb3);
    assert(h.engine.tx_completed == 2);
    for (i = 10; i < f.writes; ++i)
        if (f.address[i] == 0xfd8fe4) {
            assert(count < 7 && f.value[i] == commands[count]); ++count;
        }
    assert(count == 7);
}
static void failures(void)
{
    struct fake f;
    struct dtp_torne_hal h;
    uint8_t tx = 2;
    uint32_t i, calls;
    /* prepareの全13 I/O位置で失敗させ、それ以降のI/O停止を確認する。 */
    for (i = 1; i <= 13; ++i) {
        setup(&f,&h); f.fail_at = i;
        assert(dtp_torne_prepare(&h) == DTP_I2C_IO_ERROR);
        assert(f.calls == i && h.fault_address != 0);
        assert(dtp_torne_prepare(&h) == DTP_I2C_IO_ERROR && f.calls == i);
    }
    setup(&f,&h); f.exclusive = 0;
    assert(dtp_torne_prepare(&h) == DTP_I2C_OWNERSHIP_LOST && !f.calls);
    setup(&f,&h); f.state = 0x80;
    assert(dtp_torne_prepare(&h) == DTP_I2C_BUSY);
    assert(dtp_torne_prepare(&h) == DTP_I2C_REOPEN_REQUIRED);
    prepare(&f,&h);
    assert(dtp_torne_start(&h,9,&tx,1,0,100) == DTP_I2C_PENDING);
    f.state = 0x10;
    assert(completed(&f,&h) == DTP_I2C_ADDRESS_NACK);
    assert(dtp_torne_start(&h,9,&tx,1,0,100) == DTP_I2C_REOPEN_REQUIRED);
    prepare(&f,&h);
    assert(dtp_torne_start(&h,9,&tx,1,0,100) == DTP_I2C_PENDING);
    assert(completed(&f,&h) == DTP_I2C_PENDING);
    f.state = 0x10;
    assert(completed(&f,&h) == DTP_I2C_DATA_NACK && h.engine.tx_completed == 0);
    prepare(&f,&h);
    assert(dtp_torne_start(&h,9,&tx,1,0,100) == DTP_I2C_PENDING);
    calls = f.calls; ++f.generation;
    assert(dtp_torne_poll(&h) == DTP_I2C_OWNERSHIP_LOST && f.calls == calls);
    prepare(&f,&h);
    f.now = 0xfffffff0u;
    assert(dtp_torne_start(&h,9,&tx,1,0,32) == DTP_I2C_PENDING);
    f.now = 0x10;
    assert(dtp_torne_poll(&h) == DTP_I2C_TIMEOUT && f.control == 0x40);
    prepare(&f,&h);
    assert(dtp_torne_start(&h,9,&tx,1,0,100) == DTP_I2C_PENDING);
    f.control = 0x80;
    assert(dtp_torne_poll(&h) == DTP_I2C_CONTROLLER_ERROR);
    /* 開始時、完了時、受信データ読み取り時のI/O失敗も成功に化けない。 */
    for (i = 1; i <= 4; ++i) {
        prepare(&f,&h); f.fail_at = f.calls + i;
        assert(dtp_torne_start(&h,9,&tx,1,0,100) == DTP_I2C_IO_ERROR);
        assert(f.calls == f.fail_at);
    }
    for (i = 1; i <= 3; ++i) {
        prepare(&f,&h);
        assert(dtp_torne_start(&h,9,0,0,1,100) == DTP_I2C_PENDING);
        assert(completed(&f,&h) == DTP_I2C_PENDING);
        f.fail_at = f.calls + i;
        assert(completed(&f,&h) == DTP_I2C_IO_ERROR && f.calls == f.fail_at);
    }
}
static void lengths_and_stop(void)
{
    struct fake f;
    struct dtp_torne_hal h;
    uint8_t tx[40];
    uint32_t i, mode;
    for (i = 0; i < 40; ++i) tx[i] = (uint8_t)i;
    /* TXのみ、RXのみ、最大長の複合転送。完了bitを毎byte再評価する。 */
    for (mode = 0; mode < 3; ++mode) {
        prepare(&f,&h);
        for (i = 0; i < 40; ++i) f.input[i] = (uint8_t)(0xa0 + i);
        assert(dtp_torne_start(&h,9,tx,mode == 1 ? 0 : 40,
            mode == 0 ? 0 : 40,100) == DTP_I2C_PENDING);
        for (i = 0; h.engine.phase != DTP_I2C_STOP; ++i) {
            uint32_t writes = f.writes;
            assert(i < 100);
            assert(dtp_torne_poll(&h) == DTP_I2C_PENDING && f.writes == writes);
            /* 受信時の状態bit4を送信NACKに誤分類しない。 */
            f.state = h.engine.phase == DTP_I2C_RX ? 0x10 : 0;
            assert(completed(&f,&h) == DTP_I2C_PENDING);
        }
        assert(dtp_torne_poll(&h) == DTP_I2C_OK);
        if (mode != 1) assert(h.engine.tx_completed == 40);
        if (mode != 0) assert(h.engine.rx_completed == 40 && !memcmp(h.engine.rx,f.input,40));
    }
    prepare(&f,&h);
    assert(dtp_torne_start(&h,9,tx,1,0,10) == DTP_I2C_PENDING);
    assert(completed(&f,&h) == DTP_I2C_PENDING);
    assert(completed(&f,&h) == DTP_I2C_PENDING);
    f.state = 0x80; f.now = 10;
    assert(dtp_torne_poll(&h) == DTP_I2C_TIMEOUT);
    assert(h.engine.tx_completed == 1 && h.engine.requires_reset);
}
int main(void)
{
    transfer(); failures(); lengths_and_stop();
    puts("torne HAL: register sequence, completion, NACK, deadlines, ownership and I/O faults PASS");
    return 0;
}
