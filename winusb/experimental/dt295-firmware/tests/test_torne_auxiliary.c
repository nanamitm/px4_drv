/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_auxiliary.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fake {
    struct dtp_torne_aux c;
    struct dtp_torne_mailbox m;
    uint32_t state,control,calls,writes;
    uint8_t input[40],cursor;
};
static int read32(void *p,uint32_t a,uint32_t *v)
{
    struct fake *f=p; ++f->calls;
    switch (a) {
    case 0xfd8fe0: *v=f->state; break;
    case 0xfd8fe4: *v=f->control; break;
    case 0xfd8ff0: assert(f->cursor<40); *v=f->input[f->cursor++]; break;
    default: *v=0;
    }
    return 1;
}
static int write32(void *p,uint32_t a,uint32_t v)
{ struct fake *f=p; ++f->calls; ++f->writes; if (a==0xfd8fe4) f->control=v; return 1; }
static int write16(void *p,uint32_t a,uint16_t v) { return write32(p,a,v); }
static uint32_t now(void *p) { return ((struct fake *)p)->m.host_us; }
static int owned(void *p,uint32_t generation)
{
    struct fake *f=p;
    return generation==1 && !f->c.revoked && f->c.token && f->c.token==f->m.lease;
}
static void setup(struct fake *f)
{
    struct dtp_torne_bus bus={f,read32,write32,write16,now,owned};
    memset(f,0,sizeof(*f));
    dtp_torne_aux_init(&f->c,&f->m,&bus,DTP_TA_FEATURE_HOST_CLOCK);
    assert(f->m.magic==DTP_TA_MAGIC && f->m.abi==2 && f->m.features==2 && !f->calls);
}
static void request(struct fake *f,uint32_t op)
{
    ++f->m.request_sequence;
    f->m.operation=op;
    dtp_torne_aux_step(&f->c);
}
static void prepare(struct fake *f)
{
    setup(f); f->m.host_us=100;
    dtp_torne_aux_step(&f->c);
    f->m.lease=f->m.request_lease=17;
    request(f,DTP_TA_PREPARE);
    assert(f->m.status==DTP_TA_OK && f->m.features==3);
}
static void start(struct fake *f)
{
    f->m.address=9; f->m.tx_size=2; f->m.rx_size=2; f->m.timeout_us=1000;
    f->m.tx[0]=3; f->m.tx[1]=0x80;
    request(f,DTP_TA_I2C);
    assert(f->c.pending && f->m.response_sequence!=f->m.request_sequence);
}
int main(void)
{
    struct fake f;
    uint32_t calls,i;
    setup(&f);
    request(&f,DTP_TA_PING); assert(f.m.status==0 && !f.calls);
    request(&f,DTP_TA_PREPARE); assert(f.m.status==DTP_TA_NOT_READY && !f.calls);
    request(&f,99); assert(f.m.status==DTP_TA_UNSUPPORTED && !f.calls);
    prepare(&f); start(&f); f.input[0]=1; f.input[1]=0xb3;
    calls=f.calls; dtp_torne_aux_step(&f.c);
    assert(f.c.pending && f.calls==calls+2 && f.m.rx_completed==0);
    for (i=0;f.c.pending;++i) { assert(i<10); f.control|=1; dtp_torne_aux_step(&f.c); }
    assert(f.m.status==0 && f.m.rx[0]==1 && f.m.rx[1]==0xb3 && f.m.rx_completed==2 && f.m.tx_completed==2);
    calls=f.calls; dtp_torne_aux_step(&f.c); assert(f.calls==calls);
    request(&f,DTP_TA_QUIESCE); assert(f.m.status==0 && f.m.features==2 && f.c.revoked);
    prepare(&f); calls=f.calls; f.m.address=128; f.m.tx_size=1; f.m.timeout_us=1;
    request(&f,DTP_TA_I2C); assert(f.m.status==DTP_TA_BAD_REQUEST && f.calls==calls);
    f.m.address=9; f.m.tx_size=41;
    request(&f,DTP_TA_I2C); assert(f.m.status==DTP_TA_BAD_REQUEST && f.calls==calls);
    prepare(&f); start(&f); calls=f.calls; f.m.lease=18;
    dtp_torne_aux_step(&f.c); assert(f.m.status==DTP_TA_REVOKED && f.calls==calls && f.m.features==2);
    request(&f,DTP_TA_PREPARE); assert(f.m.status==DTP_TA_NOT_READY && f.calls==calls);
    prepare(&f); start(&f); f.c.clock_polls=DTP_TA_CLOCK_POLLS-1;
    dtp_torne_aux_step(&f.c); assert(f.m.status==DTP_TA_CLOCK_STALLED && f.control==0x40 && f.c.revoked);
    prepare(&f); f.m.host_us=0xfffffff0u; start(&f);
    f.m.host_us=0x3d8; /* 1000us後、wrapをまたぐ。 */
    dtp_torne_aux_step(&f.c); assert(f.c.pending==3 && f.m.response_sequence!=f.m.request_sequence);
    dtp_torne_aux_step(&f.c); assert(f.m.status==DTP_TA_TRANSFER_FAILED && f.m.result==DTP_I2C_TIMEOUT && f.m.features==2);
    prepare(&f); start(&f); f.state=0x10; f.control|=1;
    dtp_torne_aux_step(&f.c); assert(f.c.pending==3);
    dtp_torne_aux_step(&f.c); assert(f.m.result==DTP_I2C_ADDRESS_NACK && f.m.rx_completed==0 && !f.m.rx[0]);
    prepare(&f); start(&f); f.state=0x80; f.m.host_us+=1000;
    dtp_torne_aux_step(&f.c); assert(f.c.pending==3);
    f.m.host_us+=1000; dtp_torne_aux_step(&f.c);
    assert(f.c.pending==3 && f.m.response_sequence!=f.m.request_sequence);
    f.state=0; dtp_torne_aux_step(&f.c);
    assert(!f.c.pending && f.m.result==DTP_I2C_TIMEOUT && !(f.m.state&0x80) && f.m.features==2);
    prepare(&f); start(&f); f.state=0x80; f.m.host_us+=1000;
    dtp_torne_aux_step(&f.c); f.m.host_us+=499999; dtp_torne_aux_step(&f.c); assert(f.c.pending==3);
    f.m.host_us++; dtp_torne_aux_step(&f.c);
    assert(!f.c.pending && f.m.result==DTP_I2C_TIMEOUT && (f.m.state&0x80) && f.m.features==2);
    prepare(&f); start(&f); f.state=0x80; f.m.host_us+=1000;
    dtp_torne_aux_step(&f.c); calls=f.calls; f.m.lease=18; dtp_torne_aux_step(&f.c);
    assert(f.m.status==DTP_TA_REVOKED && f.calls==calls);
    prepare(&f); start(&f); f.m.request_sequence++;
    dtp_torne_aux_step(&f.c); assert(f.m.status==DTP_TA_REVOKED && f.c.revoked);
    prepare(&f); f.c.clock_feature=DTP_TA_FEATURE_DEVICE_CLOCK;
    f.m.timeout_us=2500000; request(&f,DTP_TA_CLOCK_WAIT);
    assert(f.c.pending==2); calls=f.calls;
    f.m.host_us+=2499999; dtp_torne_aux_step(&f.c); assert(f.c.pending==2);
    f.m.host_us++; dtp_torne_aux_step(&f.c);
    assert(f.m.status==DTP_TA_OK && !f.c.pending && f.calls==calls && f.m.features==5 && !f.m.rx_completed);
    puts("PAF2 mailbox: lease, sequence, I2C, bounds, clock stall/wrap, NACK and quiesce PASS");
    return 0;
}
