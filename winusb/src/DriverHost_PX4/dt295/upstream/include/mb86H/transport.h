/* SPDX-License-Identifier: MIT */
#ifndef MB86H_TRANSPORT_H
#define MB86H_TRANSPORT_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include "mb86H/protocol/constants.h"

typedef int (*mb86H_receive_callback)(void *, int, const uint8_t *, size_t);

/* Caller serializes operations. exchange is one attempt, never a replay.
 * receive_stop success means all callbacks are drained; failure retains
 * handle, buffer and opaque ownership. Start/stop may invoke callbacks. */
struct mb86H_transport_ops {
    int (*exchange)(void *, const uint8_t [MB86H_DEBUG_PACKET_SIZE],
                    uint8_t [MB86H_DEBUG_PACKET_SIZE]);
    int (*receive_start)(void *, uint8_t *, size_t,
                         mb86H_receive_callback, void *, void **);
    int (*receive_stop)(void *, void *);
};
struct mb86H_transport {
    const struct mb86H_transport_ops *ops;
    void *opaque;
};
#endif
