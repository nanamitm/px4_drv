/* SPDX-License-Identifier: GPL-2.0-only */
#include "i2c_engine.h"
#include "auxiliary.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
    uint32_t state, control, reads, count;
    uint32_t offsets[512], values[512];
    uint8_t input[40], cursor;
    int owned;
};

static uint32_t read32(void *opaque, uint32_t offset)
{
    struct fake *f = opaque;
    f->reads++;
    if (offset == DTP_I2C_STATE) return f->state;
    if (offset == DTP_I2C_CONTROL) return f->control;
    assert(offset == DTP_I2C_DATA && f->cursor < 40);
    return f->input[f->cursor++];
}

static void write32(void *opaque, uint32_t offset, uint32_t value)
{
    struct fake *f = opaque;
    assert(f->count < 512);
    assert(offset == DTP_I2C_CONTROL || offset == DTP_I2C_DATA);
    f->offsets[f->count] = offset;
    f->values[f->count++] = value;
    if (offset == DTP_I2C_CONTROL) f->control = value;
}

static int owned(void *opaque) { return ((struct fake *)opaque)->owned; }

static void setup(struct fake *f, struct dtp_i2c_engine *e)
{
    struct dtp_i2c_io io = {f, read32, write32, owned};
    memset(f, 0, sizeof(*f));
    f->owned = 1;
    f->control = 0x42;
    dtp_i2c_init(e, &io);
}

static uint32_t data_writes(struct fake *f, uint32_t *out)
{
    uint32_t i, count = 0;
    for (i = 0; i < f->count; ++i)
        if (f->offsets[i] == DTP_I2C_DATA) out[count++] = f->values[i];
    return count;
}

static void gates(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint8_t tx = 1;
    setup(&f, &e);
    f.owned = 0;
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0,10) == DTP_I2C_NOT_READY);
    assert(!f.reads && !f.count);
    f.owned = 1;
    assert(dtp_i2c_start(&e,0x80,&tx,1,0,0,10) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x100,&tx,1,0,0,10) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x10,0,1,0,0,10) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x10,0,0,0,0,10) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x10,&tx,41,0,0,10) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x10,&tx,1,41,0,10) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0,0) == DTP_I2C_INVALID);
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0,0x80000000u) == DTP_I2C_INVALID);
    assert(!f.reads && !f.count);
    f.state = 0x80;
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0,10) == DTP_I2C_BUSY);
    assert(!f.count);
    /* 前回のI/O定義を渡しても、コンテキスト初期化で消失しない。 */
    dtp_i2c_init(&e,&e.io);
    f.state = 0;
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0,10) == DTP_I2C_PENDING);
}

static void combined_transfer(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint8_t tx[] = {0x25,0x4d};
    uint32_t writes[80], count, i;
    setup(&f, &e);
    f.input[0] = 0x12;
    f.input[1] = 0x34;
    assert(dtp_i2c_start(&e,0x10,tx,2,2,100,100) == DTP_I2C_PENDING);
    tx[0] = 0xff; /* 非同期転送中の呼出元バッファ変更に依存しない。 */
    assert(dtp_i2c_start(&e,0x10,tx,2,2,101,100) == DTP_I2C_BUSY);
    for (i = 101; e.result == DTP_I2C_PENDING && i < 120; ++i) dtp_i2c_poll(&e,i,1);
    assert(e.result == DTP_I2C_OK && e.phase == DTP_I2C_DONE);
    assert(e.tx_completed == 2 && e.rx_completed == 2 && !e.in_flight);
    assert(e.rx[0] == 0x12 && e.rx[1] == 0x34);
    count = data_writes(&f,writes);
    assert(count == 4 && writes[0] == 0x20 && writes[1] == 0x25 && writes[2] == 0x4d && writes[3] == 0x21);
    for (i = 0; i < f.count; ++i)
        if (f.offsets[i] == DTP_I2C_CONTROL && !(f.values[i] & 0x10)) break;
    assert(i == f.count-1); /* モデル上で読み取りアドレスより前にSTOPを挟まない。 */
    assert(!e.requires_reset && e.stop_requested);
}

static void read_only_and_maximum(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint8_t tx[40];
    uint32_t i, writes[80];
    setup(&f, &e);
    for (i = 0; i < 40; ++i) f.input[i] = (uint8_t)(i+1);
    assert(dtp_i2c_start(&e,0x11,0,0,40,0,1000) == DTP_I2C_PENDING);
    for (i = 1; e.result == DTP_I2C_PENDING && i < 100; ++i) dtp_i2c_poll(&e,i,1);
    assert(e.result == DTP_I2C_OK && e.rx_completed == 40 && !e.tx_completed);
    assert(data_writes(&f,writes) == 1 && writes[0] == 0x23);
    for (i = 0; i < 40; ++i) assert(e.rx[i] == i+1);
    setup(&f, &e);
    for (i = 0; i < 40; ++i) tx[i] = (uint8_t)i;
    assert(dtp_i2c_start(&e,0x10,tx,40,40,0,1000) == DTP_I2C_PENDING);
    for (i = 1; e.result == DTP_I2C_PENDING && i < 120; ++i) dtp_i2c_poll(&e,i,1);
    assert(e.result == DTP_I2C_OK && e.tx_completed == 40 && e.rx_completed == 40);
}

static void deadline_and_stalled_completion(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint8_t tx = 7;
    uint32_t count, i;
    setup(&f, &e);
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,100,10) == DTP_I2C_PENDING);
    count = f.count;
    for (i = 101; i < 110; ++i) assert(dtp_i2c_poll(&e,i,0) == DTP_I2C_PENDING);
    assert(f.count == count && !e.tx_submitted);
    assert(dtp_i2c_poll(&e,110,0) == DTP_I2C_TIMEOUT);
    assert(e.stop_requested && e.requires_reset);
    count = f.count;
    assert(dtp_i2c_poll(&e,111,1) == DTP_I2C_TIMEOUT && f.count == count);
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,112,10) == DTP_I2C_REOPEN_REQUIRED);
    setup(&f, &e);
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0xfffffff0u,32) == DTP_I2C_PENDING);
    assert(dtp_i2c_poll(&e,0,0) == DTP_I2C_PENDING);
    assert(dtp_i2c_poll(&e,0x10,0) == DTP_I2C_TIMEOUT);
}

static void errors_and_ownership(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint8_t tx[] = {1,2};
    uint32_t count, reads;
    setup(&f, &e);
    assert(dtp_i2c_start(&e,0x10,tx,2,0,0,100) == DTP_I2C_PENDING);
    assert(dtp_i2c_poll(&e,1,1) == DTP_I2C_PENDING);
    assert(e.tx_submitted == 1 && !e.tx_completed && e.in_flight);
    f.control |= 0x80;
    assert(dtp_i2c_poll(&e,2,1) == DTP_I2C_CONTROLLER_ERROR);
    assert(!e.tx_completed && e.in_flight && e.requires_reset);
    count = f.count;
    assert(dtp_i2c_start(&e,0x10,tx,2,0,3,100) == DTP_I2C_REOPEN_REQUIRED && f.count == count);
    setup(&f, &e);
    assert(dtp_i2c_start(&e,0x10,tx,2,0,0,100) == DTP_I2C_PENDING);
    count = f.count; reads = f.reads; f.owned = 0;
    assert(dtp_i2c_poll(&e,1,1) == DTP_I2C_OWNERSHIP_LOST);
    assert(f.count == count && f.reads == reads && !e.stop_requested && e.requires_reset);
}

static void stop_completion_deadline(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint8_t tx = 1;
    setup(&f, &e);
    assert(dtp_i2c_start(&e,0x10,&tx,1,0,0,10) == DTP_I2C_PENDING);
    dtp_i2c_poll(&e,1,1);
    dtp_i2c_poll(&e,2,1);
    assert(e.phase == DTP_I2C_STOP);
    f.state |= 0x80;
    assert(dtp_i2c_poll(&e,9,0) == DTP_I2C_PENDING);
    assert(dtp_i2c_poll(&e,10,0) == DTP_I2C_TIMEOUT);
}

static void auxiliary_mailbox(void)
{
    struct dtp_aux_mailbox m;
    memset(&m,0xaa,sizeof(m));
    dtp_aux_init(&m);
    assert(m.magic == DTP_AUX_MAGIC && m.abi == 1 && !m.features && !m.request_sequence && !m.response_sequence);
    assert(!dtp_aux_step(&m) && m.heartbeat == 1);
    m.operation = DTP_AUX_PING; m.request_sequence = 1;
    assert(dtp_aux_step(&m) && m.response_sequence == 1 && m.status == DTP_AUX_OK);
    m.operation = DTP_AUX_I2C;
    assert(!dtp_aux_step(&m)); /* 同一sequenceの要求を再実行しない。 */
    m.request_sequence = 2;
    assert(dtp_aux_step(&m) && m.status == DTP_AUX_NOT_READY && !m.features);
    m.operation = 99; m.request_sequence = 3;
    assert(dtp_aux_step(&m) && m.status == DTP_AUX_UNSUPPORTED);
    m.magic = 0;
    assert(!dtp_aux_step(&m) && m.response_sequence == 3);
    dtp_aux_init(&m);
    assert(!m.request_sequence && !m.response_sequence);
}

static void event_schedules(void)
{
    struct fake f;
    struct dtp_i2c_engine e;
    uint32_t seed = 295, trial, tick, tx_size, rx_size, now, count;
    uint8_t tx[40];
    for (trial = 0; trial < 500; ++trial) {
        setup(&f,&e);
        seed = seed*1664525u+1013904223u;
        tx_size = seed%41;
        seed = seed*1664525u+1013904223u;
        rx_size = seed%41;
        if (!tx_size && !rx_size) rx_size = 1;
        memset(tx,0x55,sizeof(tx));
        now = trial&1 ? 0xffffff80u : 100;
        assert(dtp_i2c_start(&e,0x10,tx,tx_size,rx_size,now,10000) == DTP_I2C_PENDING);
        for (tick = 1; tick < 1000 && e.result == DTP_I2C_PENDING; ++tick) {
            seed = seed*1664525u+1013904223u;
            count = f.count;
            dtp_i2c_poll(&e,now+tick,(int)(seed&1));
            if (!(seed&1)) assert(f.count == count);
            assert(e.tx_completed <= e.tx_submitted && e.tx_submitted <= tx_size);
            assert(e.rx_completed <= rx_size);
        }
        assert(e.result == DTP_I2C_OK && e.tx_completed == tx_size && e.rx_completed == rx_size);
    }
}

int main(void)
{
    gates(); combined_transfer(); read_only_and_maximum(); deadline_and_stalled_completion();
    errors_and_ownership(); stop_completion_deadline(); auxiliary_mailbox(); event_schedules();
    puts("PASS: 8 I2C/auxiliary invariant groups, including 500 event schedules (simulated I/O only)");
    return 0;
}
