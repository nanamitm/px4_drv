/* SPDX-License-Identifier: GPL-2.0-only */
#include "torne_clock.h"
uint32_t dtp_torne_clock_update(struct dtp_torne_clock *c,uint32_t raw)
{
    const uint32_t frame=raw&0x7ffu;
    if (c->initialized) c->us+=((frame-c->frame)&0x7ffu)*1000u;
    else c->initialized=1;
    c->frame=frame;
    return c->us;
}
