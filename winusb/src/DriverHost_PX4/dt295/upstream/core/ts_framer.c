/* SPDX-License-Identifier: MIT */
#include "mb86H/ts_framer.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int record_sync(const uint8_t *record)
{
    return record[0] == 0x47;
}

static void consume_record(struct mb86H_ts_framer *framer)
{
    if (framer->record_size < framer->used)
        memmove(framer->storage, framer->storage + framer->record_size,
                framer->used - framer->record_size);
    framer->used -= framer->record_size;
}

static void drop(struct mb86H_ts_framer *framer, size_t count)
{
    if (count < framer->used)
        memmove(framer->storage, framer->storage + count, framer->used - count);
    framer->used -= count;
    framer->discarded_bytes += count;
}

static int find_alignment(struct mb86H_ts_framer *framer)
{
    size_t offset;

    if (framer->used < framer->record_size * 2)
        return 0;
    for (offset = 0; offset + framer->record_size * 2 <= framer->used; ++offset) {
        if (record_sync(framer->storage + offset) &&
            record_sync(framer->storage + offset + framer->record_size)) {
            drop(framer, offset);
            framer->synchronized = 1;
            return 1;
        }
    }
    drop(framer, framer->used - (framer->record_size * 2 - 1));
    return 0;
}

void mb86H_ts_framer_init(struct mb86H_ts_framer *framer)
{
    (void)mb86H_ts_framer_init_layout(framer, MB86H_TS_RECORD_SIZE);
}

int mb86H_ts_framer_init_layout(struct mb86H_ts_framer *framer, size_t record_size)
{
    if (!framer || (record_size != MB86H_TS_PACKET_SIZE &&
                    record_size != MB86H_TS_RECORD_SIZE))
        return MB86H_TS_FRAMER_ARGUMENT;
    memset(framer, 0, sizeof(*framer));
    framer->record_size = record_size;
    return MB86H_TS_FRAMER_OK;
}

int mb86H_ts_framer_push(struct mb86H_ts_framer *framer, const uint8_t *input,
                       size_t input_size, mb86H_ts_packet_sink sink, void *opaque,
                       size_t *consumed)
{
    size_t index = 0;
    int result = MB86H_TS_FRAMER_OK;

    if (!framer || (framer->record_size != MB86H_TS_PACKET_SIZE &&
                    framer->record_size != MB86H_TS_RECORD_SIZE) ||
        (!input && input_size) || !sink || !consumed)
        return MB86H_TS_FRAMER_ARGUMENT;
    *consumed = 0;
    while (index < input_size) {
        int callback_result;

        if (framer->used == MB86H_TS_FRAMER_STORAGE_SIZE) {
            if (!framer->synchronized) {
                (void)find_alignment(framer);
            } else if (!record_sync(framer->storage)) {
                framer->synchronized = 0;
                framer->desyncs++;
                drop(framer, 1);
                result = MB86H_TS_FRAMER_DESYNC;
            } else {
                callback_result = sink(opaque, framer->storage);
                if (callback_result)
                    return callback_result;
                consume_record(framer);
                framer->emitted_packets++;
            }
            continue;
        }
        framer->storage[framer->used++] = input[index++];
        *consumed = index;
        if (!framer->synchronized)
            (void)find_alignment(framer);
    }

    while (framer->synchronized && framer->used >= framer->record_size) {
        int callback_result;

        if (!record_sync(framer->storage)) {
            framer->synchronized = 0;
            framer->desyncs++;
            drop(framer, 1);
            result = MB86H_TS_FRAMER_DESYNC;
            break;
        }
        callback_result = sink(opaque, framer->storage);
        if (callback_result)
            return callback_result;
        consume_record(framer);
        framer->emitted_packets++;
    }
    return result;
}
