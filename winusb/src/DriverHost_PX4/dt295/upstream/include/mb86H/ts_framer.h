/* SPDX-License-Identifier: MIT */
#ifndef MB86H_TS_FRAMER_H
#define MB86H_TS_FRAMER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include "mb86H/protocol/constants.h"

enum {
    MB86H_TS_PACKET_SIZE = MB86H_TS_PACKET_BYTES,
    MB86H_TS_RECORD_SUFFIX_SIZE = MB86H_TS_RECORD_SUFFIX_BYTES,
    MB86H_TS_RECORD_SIZE = MB86H_TS_RECORD_BYTES,
    MB86H_TS_FRAMER_STORAGE_SIZE = MB86H_TS_FRAMER_STORAGE_BYTES,
};

enum mb86H_ts_framer_error {
    MB86H_TS_FRAMER_OK = 0,
    MB86H_TS_FRAMER_ARGUMENT = -2000,
    MB86H_TS_FRAMER_DESYNC = -2001,
};

typedef int (*mb86H_ts_packet_sink)(void *opaque,
                                  const uint8_t packet[MB86H_TS_PACKET_SIZE]);

/* One instance belongs to exactly one input stream. */
struct mb86H_ts_framer {
    uint8_t storage[MB86H_TS_FRAMER_STORAGE_SIZE];
    size_t used;
    size_t emitted_packets;
    size_t discarded_bytes;
    size_t desyncs;
    size_t record_size;
    int synchronized;
};

void mb86H_ts_framer_init(struct mb86H_ts_framer *framer);
int mb86H_ts_framer_init_layout(struct mb86H_ts_framer *framer, size_t record_size);

/*
 * Consume arbitrary chunks of raw 188-byte packets or observed 188+4 records,
 * selected by mb86H_ts_framer_init_layout().
 * The callback is called only with complete 188-byte packets.  Callback
 * failures are returned verbatim and leave that packet queued for a zero-byte
 * retry.  On return, consumed reports bytes copied from input; pass the
 * unconsumed input tail after a successful zero-byte retry.
 * MB86H_TS_FRAMER_DESYNC reports a recovered or pending discontinuity; valid
 * packets found later in this same chunk may already have been emitted.
 */
int mb86H_ts_framer_push(struct mb86H_ts_framer *framer, const uint8_t *input,
                       size_t input_size, mb86H_ts_packet_sink sink, void *opaque,
                       size_t *consumed);

#endif
