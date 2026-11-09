#include "mb86H/stream_input.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

struct received {
    unsigned count;
    int fail;
    uint8_t first[8];
};

static void packet(uint8_t output[MB86H_TS_PACKET_SIZE], uint8_t id)
{
    unsigned i;

    output[0] = 0x47;
    for (i = 1; i < MB86H_TS_PACKET_SIZE; ++i)
        output[i] = (uint8_t)(id + i);
}

static int collect(void *opaque, const uint8_t input[MB86H_TS_PACKET_SIZE])
{
    struct received *received = opaque;

    if (received->fail)
        return received->fail;
    if (received->count < sizeof(received->first))
        received->first[received->count] = input[1];
    ++received->count;
    return 0;
}

int main(void)
{
    uint8_t raw[MB86H_TS_PACKET_SIZE * 2];
    uint8_t wire[MB86H_MUX_FRAME];
    struct mb86H_stream_input input;
    struct received received = {0};
    unsigned offset;

    packet(raw, 0x10);
    packet(raw + MB86H_TS_PACKET_SIZE, 0x20);
    CHECK(!mb86H_stream_input_init(&input, MB86H_STREAM_FORMAT_RAW_TS, 1,
                                   MB86H_TS_PACKET_SIZE));
    for (offset = 0; offset < sizeof(raw);) {
        unsigned bytes = sizeof(raw) - offset;
        if (bytes > 37)
            bytes = 37;
        CHECK(!mb86H_stream_input_push(&input, 1, raw + offset, bytes,
                                       collect, &received));
        offset += bytes;
    }
    CHECK(received.count == 2 && received.first[0] == 0x11 && received.first[1] == 0x21);

    CHECK(!mb86H_stream_input_init(&input, MB86H_STREAM_FORMAT_XZM1, 0,
                                   MB86H_TS_PACKET_SIZE));
    CHECK(!mb86H_ts_mux_pack(wire, 1, 0, raw, sizeof(raw)));
    CHECK(mb86H_stream_input_push(&input, 0, wire, sizeof(wire), collect,
                                  &received) == MB86H_MUX_INVALID);
    CHECK(input.terminal_error == MB86H_MUX_INVALID);

    CHECK(!mb86H_stream_input_init(&input, MB86H_STREAM_FORMAT_XZM1, 0,
                                   MB86H_TS_PACKET_SIZE));
    received = (struct received){0};
    CHECK(!mb86H_ts_mux_pack(wire, 0, 0, raw, sizeof(raw)));
    for (offset = 0; offset < sizeof(wire);) {
        unsigned bytes = sizeof(wire) - offset;
        if (bytes > 19)
            bytes = 19;
        CHECK(!mb86H_stream_input_push(&input, 0, wire + offset, bytes,
                                       collect, &received));
        offset += bytes;
    }
    CHECK(received.count == 2 && input.framer.emitted_packets == 2);

    CHECK(!mb86H_stream_input_init(&input, MB86H_STREAM_FORMAT_RAW_TS, 0,
                                   MB86H_TS_PACKET_SIZE));
    CHECK(!mb86H_stream_input_push(&input, 0, raw, 100, collect, &received));
    mb86H_stream_input_reset(&input);
    received = (struct received){0};
    CHECK(!mb86H_stream_input_push(&input, 0, raw, sizeof(raw), collect, &received));
    CHECK(received.count == 2 && input.framer.used == 0);

    received = (struct received){.fail = -77};
    mb86H_stream_input_reset(&input);
    CHECK(mb86H_stream_input_push(&input, 0, raw, sizeof(raw), collect,
                                  &received) == -77);
    CHECK(input.terminal_error == -77);
    CHECK(mb86H_stream_input_init(&input, 0, 0, MB86H_TS_PACKET_SIZE) ==
          MB86H_TS_FRAMER_ARGUMENT);
    return 0;
}
