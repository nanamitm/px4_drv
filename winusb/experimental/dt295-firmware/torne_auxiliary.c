/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_auxiliary.h"
#ifdef DTP_TORNE_DEVICE_CLOCK
#include "torne_clock.h"
#endif

static void barrier(void) { __asm__ volatile ("" ::: "memory"); }

void dtp_torne_aux_init(struct dtp_torne_aux *c, volatile struct dtp_torne_mailbox *m,
    const struct dtp_torne_bus *bus,uint32_t clock_feature)
{
    uint32_t i;
    uint8_t *p=(uint8_t *)c;
    volatile uint8_t *b=(volatile uint8_t *)m;
    for (i=0;i<sizeof(*c);++i) p[i]=0;
    for (i=0;i<sizeof(*m);++i) b[i]=0;
    c->mailbox=m;
    c->clock_feature=clock_feature;
    dtp_torne_init(&c->hal,bus,1);
    m->abi=DTP_TA_ABI;
    m->features=c->clock_feature;
    barrier();
    m->magic=DTP_TA_MAGIC;
}

static void finish(struct dtp_torne_aux *c, uint32_t status, enum dtp_i2c_result result)
{
    volatile struct dtp_torne_mailbox *m=c->mailbox;
    uint32_t i;
    const int transfer=c->pending==1 || c->pending==3;
    for (i=0;i<40;++i) m->rx[i]=(status==DTP_TA_OK && c->pending==1 && i<c->hal.engine.rx_completed) ? c->hal.engine.rx[i] : 0;
    m->tx_completed=transfer ? c->hal.engine.tx_completed : 0;
    m->rx_completed=c->pending==1 && status==DTP_TA_OK ? c->hal.engine.rx_completed : 0;
    m->state=c->hal.engine.state;
    m->control=c->hal.engine.control;
    m->result=(uint32_t)result;
    m->status=status;
    m->features=c->clock_feature |
        ((c->hal.prepared && !c->hal.engine.requires_reset && !c->revoked) ? DTP_TA_FEATURE_I2C : 0);
    barrier();
    m->response_sequence=c->sequence;
    c->pending=0;
}

static void abort_transfer(struct dtp_torne_aux *c, uint32_t status)
{
    /* 自分のleaseが残る場合だけSTOP要求。所有者が変わった機器へ追加I/Oしない。 */
    if (c->mailbox->lease==c->token && c->hal.bus.exclusive(c->hal.bus.opaque,1))
        (void)c->hal.bus.write32(c->hal.bus.opaque,0xfd8fe4,0x40);
    c->revoked=1;
    c->hal.engine.requires_reset=1;
    finish(c,status,DTP_I2C_OWNERSHIP_LOST);
}

void dtp_torne_aux_step(struct dtp_torne_aux *c)
{
    volatile struct dtp_torne_mailbox *m=c->mailbox;
    uint32_t sequence,operation,i,now;
    enum dtp_i2c_result r;
    uint8_t tx[40];
    if (m->magic!=DTP_TA_MAGIC || m->abi!=DTP_TA_ABI) {
        if (c->pending) abort_transfer(c,DTP_TA_REVOKED);
        return;
    }
    m->heartbeat++;
    now=c->hal.bus.now_us(c->hal.bus.opaque);
    m->timer_us=now;
    if (now!=c->last_us) { c->last_us=now; c->clock_seen=1; c->clock_polls=0; }
    if (c->pending) {
        if (m->lease!=c->token || m->request_sequence!=c->sequence || m->request_lease!=c->token) {
            abort_transfer(c,DTP_TA_REVOKED);
            return;
        }
        /* 時間ではなく、時計更新が来ない場合の有限処理上限。 */
        if (++c->clock_polls>=DTP_TA_CLOCK_POLLS) {
            abort_transfer(c,DTP_TA_CLOCK_STALLED);
            return;
        }
        if (c->pending==2) {
            if ((uint32_t)(now-c->wait_started)>=c->wait_budget) finish(c,DTP_TA_OK,DTP_I2C_OK);
            return;
        }
        if (c->pending==3) {
            uint32_t state=0x80;
            /* STOP要求は非同期。失敗を公開する前に最大500msで静止を確認する。
             * 期限を越えた場合も元の失敗とbusy状態を返し、再利用は許可しない。 */
            if (!c->hal.bus.exclusive(c->hal.bus.opaque,1)) {
                abort_transfer(c,DTP_TA_REVOKED); return;
            }
            if (!c->hal.bus.read32(c->hal.bus.opaque,0xfd8fe0,&state)) {
                finish(c,DTP_TA_TRANSFER_FAILED,DTP_I2C_IO_ERROR); return;
            }
            c->hal.engine.state=state;
            if (!(state&0x80) || (uint32_t)(now-c->wait_started)>=500000u)
                finish(c,DTP_TA_TRANSFER_FAILED,c->hal.engine.result);
            return;
        }
        r=dtp_torne_poll(&c->hal);
        if (r!=DTP_I2C_PENDING && r!=DTP_I2C_OK && c->hal.engine.stop_requested && c->hal.fault==DTP_I2C_OK) {
            c->pending=3; c->wait_started=now; return;
        }
        if (r!=DTP_I2C_PENDING) finish(c,r==DTP_I2C_OK ? DTP_TA_OK : DTP_TA_TRANSFER_FAILED,r);
        return;
    }
    sequence=m->request_sequence;
    if (!sequence || sequence==m->response_sequence) return;
    c->sequence=sequence;
    operation=m->operation;
    if (operation==DTP_TA_PING || operation==DTP_TA_CAPS) { finish(c,DTP_TA_OK,DTP_I2C_OK); return; }
    if (operation!=DTP_TA_PREPARE && operation!=DTP_TA_I2C && operation!=DTP_TA_QUIESCE && operation!=DTP_TA_CLOCK_WAIT) {
        finish(c,DTP_TA_UNSUPPORTED,DTP_I2C_INVALID); return;
    }
    c->token=m->request_lease;
    if (c->revoked || !c->clock_seen || !c->token || m->lease!=c->token) {
        finish(c,DTP_TA_NOT_READY,DTP_I2C_NOT_READY); return;
    }
    if (operation==DTP_TA_CLOCK_WAIT) {
        if (c->clock_feature!=DTP_TA_FEATURE_DEVICE_CLOCK || !m->timeout_us || m->timeout_us>5000000u) {
            finish(c,DTP_TA_BAD_REQUEST,DTP_I2C_INVALID); return;
        }
        c->wait_started=now; c->wait_budget=m->timeout_us; c->pending=2; c->clock_polls=0;
        return;
    }
    if (operation==DTP_TA_PREPARE) {
        r=dtp_torne_prepare(&c->hal);
        finish(c,r==DTP_I2C_OK ? DTP_TA_OK : DTP_TA_TRANSFER_FAILED,r);
        return;
    }
    if (operation==DTP_TA_QUIESCE) {
        uint32_t state=0x80;
        const struct dtp_torne_bus *bus=&c->hal.bus;
        r=DTP_I2C_IO_ERROR;
        if (bus->exclusive(bus->opaque,1) && bus->write32(bus->opaque,0xfd8fe4,0x40) && bus->read32(bus->opaque,0xfd8fe0,&state))
            r=(state&0x80) ? DTP_I2C_BUSY : DTP_I2C_OK;
        c->revoked=1;
        finish(c,r==DTP_I2C_OK ? DTP_TA_OK : DTP_TA_TRANSFER_FAILED,r);
        return;
    }
    if (!c->hal.prepared) { finish(c,DTP_TA_NOT_READY,DTP_I2C_NOT_READY); return; }
    if (m->address>127 || m->tx_size>40 || m->rx_size>40 || (!m->tx_size && !m->rx_size) ||
        !m->timeout_us || m->timeout_us>0x7fffffffu) {
        finish(c,DTP_TA_BAD_REQUEST,DTP_I2C_INVALID); return;
    }
    for (i=0;i<40;++i) tx[i]=m->tx[i];
    barrier();
    if (sequence!=m->request_sequence || m->lease!=c->token || m->request_lease!=c->token) {
        finish(c,DTP_TA_REVOKED,DTP_I2C_OWNERSHIP_LOST); return;
    }
    r=dtp_torne_start(&c->hal,m->address,tx,m->tx_size,m->rx_size,m->timeout_us);
    c->pending=r==DTP_I2C_PENDING;
    c->clock_polls=0;
    if (!c->pending) finish(c,DTP_TA_TRANSFER_FAILED,r);
}

#ifndef DTP_TORNE_AUX_TEST
static struct dtp_torne_aux context;
#ifdef DTP_TORNE_DEVICE_CLOCK
static struct dtp_torne_clock clock_state;
static uint32_t clock_reads;
#endif
static int read32(void *p,uint32_t a,uint32_t *v)
{ (void)p; *v=*(volatile uint32_t *)(uintptr_t)a; return 1; }
static int write32(void *p,uint32_t a,uint32_t v)
{ (void)p; *(volatile uint32_t *)(uintptr_t)a=v; return 1; }
static int write16(void *p,uint32_t a,uint16_t v)
{
    uint32_t previous;
    read32(p,a,&previous);
    return write32(p,a,((uint32_t)v<<16)|(previous&0xffff));
}
static uint32_t now_us(void *p)
{
    struct dtp_torne_aux *c=p;
#ifdef DTP_TORNE_DEVICE_CLOCK
    /* EP0と同じUSBブロックへのアクセス量を抑える。間引くのは読取り頻度だけで、
     * 時刻は必ず実カウンターから延長する。ループ回数を時間に換算しない。 */
    if (!clock_state.initialized || (++clock_reads&255u)==0) {
        const uint32_t raw=*(volatile uint32_t *)(uintptr_t)DTP_TORNE_FRAME_REGISTER;
        c->mailbox->timer_ticks=raw&0x7ffu;
        (void)dtp_torne_clock_update(&clock_state,raw);
    }
    return clock_state.us;
#else
    return c->mailbox->host_us;
#endif
}
static int exclusive(void *p,uint32_t generation)
{
    struct dtp_torne_aux *c=p;
    return generation==1 && !c->revoked && c->token && c->mailbox->lease==c->token;
}
void dtp_aux_main(void)
{
    const struct dtp_torne_bus bus={&context,read32,write32,write16,now_us,exclusive};
    dtp_torne_aux_init(&context,(volatile struct dtp_torne_mailbox *)(uintptr_t)DTP_TA_ADDRESS,&bus,
#ifdef DTP_TORNE_DEVICE_CLOCK
        DTP_TA_FEATURE_DEVICE_CLOCK
#else
        DTP_TA_FEATURE_HOST_CLOCK
#endif
    );
    for (;;) dtp_torne_aux_step(&context);
}
#endif
