/* SPDX-License-Identifier: MIT */
/* Transport-neutral resident command session. */
#ifndef MB86H_SESSION_H
#define MB86H_SESSION_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 uint8_t;
typedef u32 uint32_t;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#include "mb86H/frontend.h"

struct mb86H_device_overview;

struct mb86H_session_ops {
    int (*exchange)(void *opaque, const uint8_t request[MB86H_DEBUG_PACKET_SIZE],
                    uint8_t response[MB86H_DEBUG_PACKET_SIZE]);
    void (*observe)(void *opaque, const uint8_t request[64],
                    const uint8_t *response, int result);
    uint32_t (*now_ms)(void *opaque);
    void (*delay_ms)(void *opaque, unsigned int milliseconds);
};

struct mb86H_session {
    struct mb86H_dvb_request request;
    unsigned int source;
    uint32_t generation;
    uint32_t sequence;
    bool request_valid;
    uint32_t tune_timeout_ms; /* Device acquisition deadline; zero uses the default. */
    uint32_t tune_poll_ms; /* Foreground cadence; zero preserves the 10 ms default. */
    bool streaming;
    bool stop_pending;
};

void mb86H_session_reset(struct mb86H_session *session);
void mb86H_session_begin(struct mb86H_session *session,
                            unsigned int source, uint32_t generation);
bool mb86H_session_matches(const struct mb86H_session *session,
                              unsigned int source);
void mb86H_session_set_streaming(struct mb86H_session *session,
                                    bool streaming);
/* Admit a resident only after its versioned public capability snapshot is
 * valid. All ordinary host sessions use DEVICE_QUERY v1; there is no native
 * STATUS-layout compatibility path. The caller may retain the overview for
 * source admission, but this function performs no device mutation. */
int mb86H_session_prepare(struct mb86H_session *,
    const struct mb86H_session_ops *, void *, struct mb86H_device_overview *);
/* INITIALIZE remains a semantic resident command; no retry follows an
 * ambiguous exchange. A successful reply is settled through the selected
 * state contract with generation zero. */
int mb86H_session_initialize(struct mb86H_session *,
    const struct mb86H_session_ops *, void *);
/* SHUTDOWN is sent once, even when no tune was admitted.  It then advances
 * SERVICE until the queried owner is fully released; errors remain raw. */
int mb86H_session_shutdown(struct mb86H_session *,
    const struct mb86H_session_ops *, void *);
/* Same one-shot SHUTDOWN and bounded owner drain as session_shutdown.  A
 * released cold image is detachable only when firmware explicitly reports
 * that the same verified image may initialize again; no RAM restore follows. */
int mb86H_session_detach(struct mb86H_session *,
    const struct mb86H_session_ops *, void *);
int mb86H_session_exchange(struct mb86H_session *session,
                               const struct mb86H_session_ops *ops, void *opaque,
                               uint32_t operation, uint32_t address,
                               uint32_t argument, uint32_t budget,
                               const uint8_t data[MB86H_DEBUG_PACKET_DATA_SIZE],
                               uint8_t response[MB86H_DEBUG_PACKET_SIZE]);
int mb86H_session_command(struct mb86H_session *session,
                              const struct mb86H_session_ops *ops, void *opaque,
                              uint32_t command, uint32_t channel,
                              const uint8_t data[MB86H_DEBUG_PACKET_DATA_SIZE],
                              uint8_t response[MB86H_DEBUG_PACKET_SIZE]);
int mb86H_session_finish_cleanup(struct mb86H_session *,
    const struct mb86H_session_ops *, void *, unsigned int, uint8_t response[64]);
int mb86H_session_settle(struct mb86H_session *session,
                             const struct mb86H_session_ops *ops, void *opaque,
                             unsigned int source, uint32_t generation,
                             bool ready, bool streaming);
int mb86H_session_tune(struct mb86H_session *session,
                           const struct mb86H_session_ops *ops, void *opaque,
                           unsigned int source,
                           const struct mb86H_dvb_request *request,
                           bool stream);
int mb86H_session_stream(struct mb86H_session *,
    const struct mb86H_session_ops *, void *, unsigned source, bool enable);
/* Called before starting a new RX owner, never on the close path. */
int mb86H_session_wait_stopped(struct mb86H_session *,
    const struct mb86H_session_ops *, void *);
/* Returns after STOP is accepted. The caller drains its RX locally; FW owns
 * subsequent capture quiescence and RF retention. This does not unload FW. */
int mb86H_session_stop(struct mb86H_session *session,
                           const struct mb86H_session_ops *ops, void *opaque,
                           unsigned int source);
int mb86H_session_service(struct mb86H_session *session,
                              const struct mb86H_session_ops *ops, void *opaque);
int mb86H_session_locked(struct mb86H_session *session,
                             const struct mb86H_session_ops *ops, void *opaque,
                             unsigned int source, bool *locked);

#endif
