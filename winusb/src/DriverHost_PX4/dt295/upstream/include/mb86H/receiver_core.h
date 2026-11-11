/* SPDX-License-Identifier: MIT */
#ifndef MB86H_TS_RECEIVER_CORE_H
#define MB86H_TS_RECEIVER_CORE_H

#include "mb86H/ts_framer.h"
#include "mb86H/ts_mux.h"

/*
 * Receiver state machine shared by user-space and a future kmod. Contains no
 * internal locks; the caller owns storage and serializes every core call.
 */
enum mb86H_ts_receiver_result {
    MB86H_TS_RECEIVER_ARGUMENT = -2300,
    MB86H_TS_RECEIVER_STOPPED = -2301,
    MB86H_TS_RECEIVER_OVERFLOW = -2302,
    MB86H_TS_RECEIVER_BUSY = -2303,
    MB86H_TS_RECEIVER_NOMEM = -2304,
};

struct mb86H_ts_receiver_stats {
    uint64_t generation;
    size_t input_bytes, packets, read_packets, queued_packets;
    size_t discarded_bytes, desyncs;
    int active, error;
};

struct mb86H_ts_receiver_core {
    struct mb86H_ts_framer framer;
    struct mb86H_ts_receiver_stats stats;
    uint8_t *packets;
    size_t capacity, head, used;
    int stopping, discard_mux_frame;
    unsigned int expected_source; /* CHANNEL_COUNT accepts FW-selected mux tags. */
};

int mb86H_ts_receiver_core_init(struct mb86H_ts_receiver_core *core,
                              uint8_t *storage, size_t capacity);
int mb86H_ts_receiver_core_start(struct mb86H_ts_receiver_core *core, size_t record_size);
void mb86H_ts_receiver_core_start_failed(struct mb86H_ts_receiver_core *core, int result);
void mb86H_ts_receiver_core_receive(struct mb86H_ts_receiver_core *core, int result,
                                  const uint8_t *data, size_t size);
int mb86H_ts_receiver_core_waiting(const struct mb86H_ts_receiver_core *core,
                                 uint64_t generation);
uint64_t mb86H_ts_receiver_core_generation(const struct mb86H_ts_receiver_core *core);
int mb86H_ts_receiver_core_read(struct mb86H_ts_receiver_core *core,
                              uint64_t generation, uint8_t *output,
                              size_t capacity, size_t *size);
void mb86H_ts_receiver_core_stop_begin(struct mb86H_ts_receiver_core *core);
void mb86H_ts_receiver_core_stop_finish(struct mb86H_ts_receiver_core *core, int result);
void mb86H_ts_receiver_core_discard_buffered(struct mb86H_ts_receiver_core *core);
void mb86H_ts_receiver_core_stats(const struct mb86H_ts_receiver_core *core,
                                struct mb86H_ts_receiver_stats *stats);
/* Decode the single firmware-selected stream; source tags are wire metadata. */
int mb86H_ts_receiver_mux_receive(struct mb86H_ts_mux *mux,
                                struct mb86H_ts_receiver_core *core,
                                const uint8_t *data,size_t size);

#endif
