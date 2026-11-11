/* SPDX-License-Identifier: MIT */
#ifndef MB86H_TS_RECEIVER_H
#define MB86H_TS_RECEIVER_H

#include "mb86H/transport.h"
#include "mb86H/receiver_core.h"
#include "mb86H/protocol/device_query.h"

struct mb86H_ts_receiver;

enum mb86H_ts_layout {
    MB86H_TS_LAYOUT_OBSERVED_188_PLUS_4 = 1,
    MB86H_TS_LAYOUT_RAW_188 = 2,
    MB86H_TS_LAYOUT_XZM1_188 = 3,
};

/* Borrows transport. Queue capacity is 2..32768 packets. */
int mb86H_ts_receiver_create(const struct mb86H_transport *transport, size_t queue_packets,
                           struct mb86H_ts_receiver **out);
int mb86H_ts_receiver_start(struct mb86H_ts_receiver *receiver,
                          enum mb86H_ts_layout layout);
/* Start from a successful DEVICE_QUERY SOURCE response. The caller admits
 * overview.stream_mask and owns TUNE/STOP/SERVICE on the same transport.
 * Rejects RF-only descriptors; validates XZM1 source tags when selected. */
int mb86H_ts_receiver_start_source(struct mb86H_ts_receiver *receiver,
    unsigned int source, const struct mb86H_source_descriptor *descriptor);
/* Blocks until data, stop or terminal error. capacity is bytes, >=188.
 * size is always a multiple of 188. Queued valid data precedes errors.
 * generation identifies the session; never combine different generations.
 * A read already waiting when stop occurs returns STOPPED, even on restart. */
int mb86H_ts_receiver_read(struct mb86H_ts_receiver *receiver,
                         uint8_t *output, size_t capacity, size_t *size,
                         uint64_t *generation);
int mb86H_ts_receiver_stats(struct mb86H_ts_receiver *receiver,
                          struct mb86H_ts_receiver_stats *stats);
/* Drop queued packets and incomplete framing before a capture window. */
int mb86H_ts_receiver_discard_buffered(struct mb86H_ts_receiver *receiver);
int mb86H_ts_receiver_stop(struct mb86H_ts_receiver *receiver);
/* No API calls/readers may remain during destroy. On failure keep and retry. */
int mb86H_ts_receiver_destroy(struct mb86H_ts_receiver *receiver);

/* One FW-selected GR or BS/CS stream with common framing. Serialize start/stop.
 * Stop cancels and drains the transport. Callbacks never invoke application I/O.
 * Firmware owns S/T routing and must exclude old in-flight output on retune.
 * Reset FW sequences before a new transport session. */
#endif
