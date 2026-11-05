/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_auxiliary.h"

static void barrier(void) { __asm__ volatile ("" ::: "memory"); }

void dtp_torne_aux_init(struct dtp_torne_aux *c, volatile struct dtp_torne_mailbox *m,
    const struct dtp_torne_bus *bus)
{
    uint32_t i;
    uint8_t *p=(uint8_t *)c;
    volatile uint8_t *b=(volatile uint8_t *)m;
    for (i=0;i<sizeof(*c);++i) p[i]=0;
    for (i=0;i<sizeof(*m);++i) b[i]=0;
    c->mailbox=m;
    dtp_torne_init(&c->hal,bus,1);
    m->abi=DTP_TA_ABI;
    m->features=DTP_TA_FEATURE_HOST_CLOCK;
    barrier();
    m->magic=DTP_TA_MAGIC;
}

static void finish(struct dtp_torne_aux *c, uint32_t status, enum dtp_i2c_result result)
{
    volatile struct dtp_torne_mailbox *m=c->mailbox;
    uint32_t i;
    for (i=0;i<40;++i) m->rx[i]=(status==DTP_TA_OK && c->pending && i<c->hal.engine.rx_completed) ? c->hal.engine.rx[i] : 0;
    m->tx_completed=c->pending ? c->hal.engine.tx_completed : 0;
    m->rx_completed=c->pending && status==DTP_TA_OK ? c->hal.engine.rx_completed : 0;
    m->state=c->hal.engine.state;
    m->control=c->hal.engine.control;
    m->result=(uint32_t)result;
    m->status=status;
    m->features=DTP_TA_FEATURE_HOST_CLOCK |
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
    now=m->host_us;
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
        r=dtp_torne_poll(&c->hal);
        if (r!=DTP_I2C_PENDING) finish(c,r==DTP_I2C_OK ? DTP_TA_OK : DTP_TA_TRANSFER_FAILED,r);
        return;
    }
    sequence=m->request_sequence;
    if (!sequence || sequence==m->response_sequence) return;
    c->sequence=sequence;
    operation=m->operation;
    if (operation==DTP_TA_PING || operation==DTP_TA_CAPS) { finish(c,DTP_TA_OK,DTP_I2C_OK); return; }
    if (operation!=DTP_TA_PREPARE && operation!=DTP_TA_I2C && operation!=DTP_TA_QUIESCE) {
        finish(c,DTP_TA_UNSUPPORTED,DTP_I2C_INVALID); return;
    }
    c->token=m->request_lease;
    if (c->revoked || !c->clock_seen || !c->token || m->lease!=c->token) {
        finish(c,DTP_TA_NOT_READY,DTP_I2C_NOT_READY); return;
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
{ return ((struct dtp_torne_aux *)p)->mailbox->host_us; }
static int exclusive(void *p,uint32_t generation)
{
    struct dtp_torne_aux *c=p;
    return generation==1 && !c->revoked && c->token && c->mailbox->lease==c->token;
}
void dtp_aux_main(void)
{
    const struct dtp_torne_bus bus={&context,read32,write32,write16,now_us,exclusive};
    dtp_torne_aux_init(&context,(volatile struct dtp_torne_mailbox *)(uintptr_t)DTP_TA_ADDRESS,&bus);
    for (;;) dtp_torne_aux_step(&context);
}
#endif
