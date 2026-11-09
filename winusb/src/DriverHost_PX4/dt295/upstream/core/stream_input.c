/* SPDX-License-Identifier: MIT */
#include "mb86H/stream_input.h"

struct stream_payload {
    struct mb86H_stream_input *input;
    mb86H_stream_packet_sink sink;
    void *opaque;
};

static int framed_packet(void *opaque,
                         const uint8_t packet[MB86H_TS_PACKET_SIZE])
{
    struct stream_payload *payload = opaque;
    return payload->sink(payload->opaque, packet);
}

static int frame_payload(struct stream_payload *payload, const uint8_t *data,
                         size_t size)
{
    size_t consumed;
    int rc = mb86H_ts_framer_push(&payload->input->framer, data, size,
                                  framed_packet, payload, &consumed);

    /* The framer retains its resynchronization state.  A fully consumed
     * desync is observable in its stats but does not terminate reception. */
    if (rc == MB86H_TS_FRAMER_DESYNC && consumed == size)
        return 0;
    return rc;
}

static int mux_payload(void *opaque, unsigned int source,
                       const uint8_t *data, size_t size)
{
    struct stream_payload *payload = opaque;

    if (source != payload->input->expected_source)
        return MB86H_MUX_INVALID;
    return frame_payload(payload, data, size);
}

int mb86H_stream_input_init(struct mb86H_stream_input *input,
                            unsigned int format, unsigned int expected_source,
                            size_t record_size)
{
    int rc;

    if (!input || expected_source >= MB86H_CHANNEL_COUNT ||
        (format != MB86H_STREAM_FORMAT_XZM1 &&
         format != MB86H_STREAM_FORMAT_RAW_TS))
        return MB86H_TS_FRAMER_ARGUMENT;
    rc = mb86H_ts_framer_init_layout(&input->framer, record_size);
    if (rc)
        return rc;
    input->format = format;
    input->expected_source = expected_source;
    input->terminal_error = 0;
    mb86H_ts_mux_reset(&input->mux);
    return 0;
}

void mb86H_stream_input_reset(struct mb86H_stream_input *input)
{
    size_t record_size;

    if (!input)
        return;
    record_size = input->framer.record_size;
    if (record_size != MB86H_TS_PACKET_SIZE &&
        record_size != MB86H_TS_RECORD_SIZE)
        return;
    mb86H_ts_mux_reset(&input->mux);
    (void)mb86H_ts_framer_init_layout(&input->framer, record_size);
    input->terminal_error = 0;
}

int mb86H_stream_input_push(struct mb86H_stream_input *input,
                            unsigned int source, const uint8_t *data,
                            size_t size, mb86H_stream_packet_sink sink,
                            void *opaque)
{
    struct stream_payload payload;
    int rc;

    if (!input || (!data && size) || !sink || input->terminal_error)
        return input && input->terminal_error ? input->terminal_error :
            MB86H_TS_FRAMER_ARGUMENT;
    payload = (struct stream_payload){input, sink, opaque};
    if (input->format == MB86H_STREAM_FORMAT_XZM1)
        rc = mb86H_ts_mux_feed(&input->mux, data, size, mux_payload, &payload);
    else if (input->format == MB86H_STREAM_FORMAT_RAW_TS) {
        if (source != input->expected_source)
            rc = MB86H_MUX_INVALID;
        else
            rc = frame_payload(&payload, data, size);
    } else
        rc = MB86H_TS_FRAMER_ARGUMENT;
    if (rc)
        input->terminal_error = rc;
    return rc;
}
