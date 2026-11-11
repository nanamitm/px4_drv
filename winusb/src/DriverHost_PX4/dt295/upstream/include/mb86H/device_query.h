/* SPDX-License-Identifier: MIT */
#ifndef MB86H_DEVICE_QUERY_H
#define MB86H_DEVICE_QUERY_H
#include "mb86H/session.h"
#include "mb86H/protocol/device_query.h"

/* Same caller-serialized command owner and sequence as tune/card/stop.
 * A query never starts a device, advances SERVICE or retries an exchange.
 * Negative transport or firmware results are preserved; a successful state
 * query may report a failed device in error/owner_error and retained flags.
 * This explicit v1 interface has no legacy-layout or unknown-format fallback. */
int mb86H_device_query_overview(struct mb86H_session *,const struct mb86H_session_ops *,
    void *,struct mb86H_device_overview *);
int mb86H_device_query_source(struct mb86H_session *,const struct mb86H_session_ops *,
    void *,unsigned,struct mb86H_source_descriptor *);
int mb86H_device_query_state(struct mb86H_session *,const struct mb86H_session_ops *,
    void *,unsigned,struct mb86H_source_state *);
/* Grants exactly one firmware capture window.  The response must echo the
 * requested version, generation, sequence and record count; it is never
 * retried because dispatch ambiguity could already have advanced the window. */
int mb86H_device_stream_window(struct mb86H_session *,const struct mb86H_session_ops *,
    void *,unsigned,uint32_t,uint32_t,unsigned);
#endif
