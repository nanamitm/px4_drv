/* SPDX-License-Identifier: MIT */
#ifndef MB86H_STREAM_INPUT_H
#define MB86H_STREAM_INPUT_H

#include "mb86H/ts_mux.h"

typedef int (*mb86H_stream_packet_sink)(void *opaque,
    const uint8_t packet[MB86H_TS_PACKET_SIZE]);

/* One selected DVB source enters through either resident XZM1 framing or a
 * transport's explicitly attributed raw TS stream.  The object is owned by
 * the adapter/transport session and must be reset before a new capture. */
struct mb86H_stream_input {
    struct mb86H_ts_mux mux;
    struct mb86H_ts_framer framer;
    unsigned int format;
    unsigned int expected_source;
    int terminal_error;
};

int mb86H_stream_input_init(struct mb86H_stream_input *, unsigned int format,
                            unsigned int expected_source, size_t record_size);
void mb86H_stream_input_reset(struct mb86H_stream_input *);
int mb86H_stream_input_push(struct mb86H_stream_input *, unsigned int source,
                            const uint8_t *data, size_t size,
                            mb86H_stream_packet_sink, void *opaque);

#endif
