/* SPDX-License-Identifier: MIT */
#include "mb86H/ts_mux.h"
#include "mb86H/bytes.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

void mb86H_ts_mux_reset(struct mb86H_ts_mux *mux)
{
    if (mux) {
        mux->used = 0;
        for (unsigned i = 0; i < MB86H_CHANNEL_COUNT; ++i)
            mux->next[i] = 0;
        mux->error = 0;
    }
}

int mb86H_ts_mux_pack(uint8_t frame[MB86H_MUX_FRAME], unsigned channel,
                    uint32_t sequence, const uint8_t *payload, size_t size)
{
    size_t i;

    if (!frame || !payload || channel >= MB86H_CHANNEL_COUNT || !size ||
        size > MB86H_MUX_PAYLOAD)
        return MB86H_MUX_INVALID;
    mb86H_be32_put(frame, MB86H_MUX_MAGIC);
    mb86H_be32_put(frame + 4, channel);
    mb86H_be32_put(frame + 8, sequence);
    mb86H_be32_put(frame + 12, (uint32_t)size);
    for (i = 0; i < size; ++i)
        frame[MB86H_MUX_HEADER + i] = payload[i];
    for (; i < MB86H_MUX_PAYLOAD; ++i)
        frame[MB86H_MUX_HEADER + i] = 0;
    return 0;
}

static int consume_frame(struct mb86H_ts_mux *mux, const uint8_t frame[MB86H_MUX_FRAME],
                         mb86H_mux_sink sink, void *opaque)
{
    uint32_t channel = mb86H_be32_get(frame + 4);
    uint32_t sequence = mb86H_be32_get(frame + 8);
    uint32_t length = mb86H_be32_get(frame + 12);
    int result;

    if (mb86H_be32_get(frame) != MB86H_MUX_MAGIC ||
        channel >= MB86H_CHANNEL_COUNT || !length || length > MB86H_MUX_PAYLOAD)
        return mux->error = MB86H_MUX_INVALID;
    if (sequence != mux->next[channel])
        return mux->error = MB86H_MUX_SEQUENCE;
    result = sink(opaque, channel, frame + MB86H_MUX_HEADER, length);
    if (result)
        return mux->error = result;
    ++mux->next[channel];
    return 0;
}

int mb86H_ts_mux_feed(struct mb86H_ts_mux *mux, const uint8_t *data, size_t size,
                    mb86H_mux_sink sink, void *opaque)
{
    size_t offset = 0;

    if (!mux || (!data && size) || !sink)
        return MB86H_MUX_INVALID;
    if (mux->error)
        return mux->error;
    while (offset < size) {
        size_t bytes;
        int result;

        if (!mux->used && size - offset >= MB86H_MUX_FRAME) {
            result = consume_frame(mux, data + offset, sink, opaque);
            if (result)
                return result;
            offset += MB86H_MUX_FRAME;
            continue;
        }
        bytes = MB86H_MUX_FRAME - mux->used;
        if (bytes > size - offset)
            bytes = size - offset;
        memcpy(mux->frame + mux->used, data + offset, bytes);
        mux->used += bytes;
        offset += bytes;
        if (mux->used != MB86H_MUX_FRAME)
            continue;
        result = consume_frame(mux, mux->frame, sink, opaque);
        if (result)
            return mux->error = result;
        mux->used = 0;
    }
    return 0;
}

int mb86H_ts_mux_finish(struct mb86H_ts_mux *mux)
{
    if (!mux)
        return MB86H_MUX_INVALID;
    if (mux->error)
        return mux->error;
    return mux->used ? (mux->error = MB86H_MUX_INVALID) : 0;
}
