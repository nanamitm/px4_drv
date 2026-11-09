/* SPDX-License-Identifier: MIT */
#ifndef MB86H_TS_MUX_H
#define MB86H_TS_MUX_H
#include "mb86H/ts_framer.h"
#include "mb86H/protocol/constants.h"
typedef int (*mb86H_mux_sink)(void *,unsigned,const uint8_t *,size_t);
struct mb86H_ts_mux {
    uint8_t frame[MB86H_MUX_FRAME];
    size_t used;
    uint32_t next[2];
    int error;
};
/* One transport session starts both channel sequences at zero. Call reset
 * after USB cancellation/drain, before starting a new session. Padding outside
 * the declared payload is ignored. The sink consumes its buffer synchronously. */
void mb86H_ts_mux_reset(struct mb86H_ts_mux *);
int mb86H_ts_mux_pack(uint8_t frame[MB86H_MUX_FRAME],unsigned channel,
                    uint32_t sequence,const uint8_t *payload,size_t size);
int mb86H_ts_mux_feed(struct mb86H_ts_mux *,const uint8_t *,size_t,
                    mb86H_mux_sink,void *);
int mb86H_ts_mux_finish(struct mb86H_ts_mux *);
#endif
