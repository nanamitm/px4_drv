// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stddef.h>
#include <string.h>
#endif

#include "ts_sync.h"

#define PX4_TS_PACKET_SIZE 188
#define PX4_TS_SYNC_WINDOW (4 * PX4_TS_PACKET_SIZE)

struct px4_ts_plain_framer {
	unsigned char pending[PX4_TS_SYNC_WINDOW];
	size_t used;
	int locked;
};

typedef void (*px4_ts_plain_sink)(void *ctx, const unsigned char *data, size_t len);

static inline void px4_ts_plain_reset(struct px4_ts_plain_framer *framer)
{
	framer->used = 0;
	framer->locked = 0;
}

static inline int px4_ts_plain_candidate(const unsigned char *data)
{
	return px4_ts_has_plain_sync(data[0]) &&
		px4_ts_has_plain_sync(data[PX4_TS_PACKET_SIZE]) &&
		px4_ts_has_plain_sync(data[2 * PX4_TS_PACKET_SIZE]) &&
		px4_ts_has_plain_sync(data[3 * PX4_TS_PACKET_SIZE]);
}

static inline void px4_ts_plain_feed(struct px4_ts_plain_framer *framer,
				     const unsigned char *data, size_t len,
				     px4_ts_plain_sink sink, void *ctx)
{
	while (len) {
		size_t need;

		/* 同期済みなら1パケットずつ確認する。4個必要なのは同期を探すときだけ。 */
		need = framer->locked ? PX4_TS_PACKET_SIZE : PX4_TS_SYNC_WINDOW;
		if (framer->used || len < need) {
			size_t take = need - framer->used;

			if (take > len)
				take = len;
			memcpy(framer->pending + framer->used, data, take);
			framer->used += take;
			data += take;
			len -= take;
			if (framer->used < need)
				break;

			if ((framer->locked && px4_ts_has_plain_sync(framer->pending[0])) ||
			    (!framer->locked && px4_ts_plain_candidate(framer->pending))) {
				sink(ctx, framer->pending, need);
				framer->used = 0;
				framer->locked = 1;
			} else if (framer->locked) {
				/* 破損したパケット内からも次の同期位置を探せるように保持する。 */
				framer->locked = 0;
			} else {
				size_t skip = 1;

				while (skip < framer->used && !px4_ts_has_plain_sync(framer->pending[skip]))
					++skip;
				framer->used -= skip;
				memmove(framer->pending, framer->pending + skip, framer->used);
			}
			continue;
		}

		if (!framer->locked) {
			if (!px4_ts_plain_candidate(data)) {
				++data;
				--len;
				continue;
			}
			framer->locked = 1;
		}

		/* 入力中の連続する完全なパケットはコピーせずまとめて渡す。 */
		need = 0;
		while (len - need >= PX4_TS_PACKET_SIZE && px4_ts_has_plain_sync(data[need]))
			need += PX4_TS_PACKET_SIZE;
		if (need) {
			sink(ctx, data, need);
			data += need;
			len -= need;
		}
		if (len >= PX4_TS_PACKET_SIZE)
			framer->locked = 0;
	}
}
