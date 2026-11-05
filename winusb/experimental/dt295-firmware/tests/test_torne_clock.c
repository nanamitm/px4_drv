/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_clock.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct dtp_torne_clock c={0};
    unsigned i;
    assert(dtp_torne_clock_update(&c,2046)==0);
    assert(dtp_torne_clock_update(&c,2047)==1000);
    assert(dtp_torne_clock_update(&c,0)==2000);
    assert(dtp_torne_clock_update(&c,0xfffff801u)==3000);
    assert(dtp_torne_clock_update(&c,1)==3000);
    for (i=2;i<10000;++i) (void)dtp_torne_clock_update(&c,i&2047);
    assert(c.us==10001000);
    c.us=0xfffffff0u; c.frame=100;
    assert(dtp_torne_clock_update(&c,101)==984);
    puts("USB frame clock: 11-bit wrap, multiple wraps, high bits, freeze and uint32 time wrap PASS");
    return 0;
}
