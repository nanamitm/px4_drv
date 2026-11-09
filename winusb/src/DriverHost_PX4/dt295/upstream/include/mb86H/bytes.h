/* SPDX-License-Identifier: MIT */
/* Big-endian word helpers shared by core and userland.  Header-only so the
 * same definitions serve the Linux module and the userspace test binaries. */
#ifndef MB86H_BYTES_H
#define MB86H_BYTES_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

static inline uint32_t mb86H_be32_get(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void mb86H_be32_put(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

#endif
