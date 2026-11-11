/* SPDX-License-Identifier: MIT */
#include "mb86H/session.h"
#include "mb86H/device_query.h"
#include "mb86H/protocol.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

void mb86H_session_reset(struct mb86H_session *s)
{
    if (s)
        *s = (struct mb86H_session){0};
}

void mb86H_session_begin(struct mb86H_session *s, unsigned source,
                           uint32_t generation)
{
    if (!s) return;
    s->source = source;
    s->generation = generation;
    s->request_valid = generation != 0 && source < MB86H_CHANNEL_COUNT;
    s->streaming = false;
    s->stop_pending = false;
}

bool mb86H_session_matches(const struct mb86H_session *s, unsigned source)
{
    return s && s->request_valid && s->source == source;
}

void mb86H_session_set_streaming(struct mb86H_session *s, bool streaming)
{
    if (s) s->streaming = streaming;
}

int mb86H_session_exchange(struct mb86H_session *s,
                              const struct mb86H_session_ops *ops, void *opaque,
                              uint32_t operation, uint32_t address,
                              uint32_t argument, uint32_t budget,
                              const uint8_t data[40], uint8_t response[64])
{
    uint8_t packet[64];
    uint32_t sequence;
    int rc, transport_rc;

    if (!s || !ops || !ops->exchange || !response) return -EINVAL;
    sequence = ++s->sequence;
    mb86H_debug_packet_pack(packet, MB86H_DEBUG_MAGIC, sequence,
                               operation, address, argument, budget, data);
    transport_rc = ops->exchange(opaque, packet, response);
    rc = transport_rc;
    if (!rc && !mb86H_debug_response_matches(response, MB86H_DEBUG_MAGIC, sequence))
        rc = -EPROTO;
    if (ops->observe)
        ops->observe(opaque, packet, transport_rc ? NULL : response, rc);
    return rc;
}

int mb86H_session_command(struct mb86H_session *s,
                             const struct mb86H_session_ops *ops, void *opaque,
                             uint32_t command, uint32_t channel,
                             const uint8_t data[40], uint8_t response[64])
{
    if (!ops) return -EINVAL;
    /* Some commands use this field as a selector, not a frontend index. */
    return mb86H_session_exchange(s, ops, opaque,
        MB86H_DEBUG_RESIDENT_DISPATCH, command, channel,
        ops->now_ms ? ops->now_ms(opaque) : 0, data, response);
}

static bool response_ok(const uint8_t response[64])
{
    return mb86H_debug_u32be_get(response + 8) == MB86H_DEBUG_OK;
}

static bool polling_valid(const struct mb86H_session *s,
                           const struct mb86H_session_ops *ops)
{
    return s && ops && ops->exchange && ops->now_ms && ops->delay_ms;
}

static int response_error(const uint8_t response[64])
{
    int rc;
    if (response_ok(response)) return 0;
    rc = (int32_t)mb86H_debug_u32be_get(response + 20);
    return rc < 0 ? rc : -EIO;
}

static int queried_state(struct mb86H_session *s,
                         const struct mb86H_session_ops *ops, void *opaque,
                         unsigned source, struct mb86H_source_state *state)
{
    int rc = mb86H_device_query_state(s, ops, opaque, source, state);
    if (rc) return rc;
    if (state->owner_error < 0) return state->owner_error;
    return state->error < 0 ? state->error : 0;
}

/* SHUTDOWN retires the device owner, not a completed RF request. A source
 * fault can legitimately remain observable after the board has restored, so
 * this path must read the raw v1 state and gate only on owner-level failure. */
static int queried_shutdown_state(struct mb86H_session *s,
                                  const struct mb86H_session_ops *ops,
                                  void *opaque,
                                  struct mb86H_source_state *state)
{
    int rc=mb86H_device_query_state(s,ops,opaque,0,state);
    if(rc)return rc;
    return state->owner_error<0?state->owner_error:0;
}

static bool shutdown_released(const struct mb86H_source_state *state)
{
    return !(state->flags&(MB86H_DEVICE_STATE_STARTED|
                           MB86H_DEVICE_STATE_INITIALIZED|
                           MB86H_DEVICE_STATE_SHUTTING_DOWN|
                           MB86H_DEVICE_STATE_RESTORE_PENDING|
                           MB86H_DEVICE_STATE_OWNED|
                           MB86H_DEVICE_STATE_CAPTURING|
                           MB86H_DEVICE_STATE_STREAM_READY)) &&
           !state->capture_mask;
}

static bool query_complete(const struct mb86H_source_state *state,
                           unsigned source, uint32_t generation, bool ready,
                           bool streaming)
{
    /* INITIALIZE establishes the board baseline before any RF request exists.
     * The shared scheduler truthfully remains QUIET at generation zero. */
    bool initialized_idle = !generation && !ready && !streaming &&
        !state->generation && state->phase == MB86H_RESIDENT_PHASE_QUIET;
    if ((!initialized_idle && state->phase != MB86H_RESIDENT_PHASE_COMPLETE) ||
        !(state->flags & MB86H_DEVICE_STATE_INITIALIZED) ||
        (state->flags & (MB86H_DEVICE_STATE_SHUTTING_DOWN |
                         MB86H_DEVICE_STATE_RESTORE_PENDING)) ||
        (!!(state->flags & MB86H_DEVICE_STATE_READY) != ready))
        return false;
    if (generation && state->generation != generation)
        return false;
    if (!!(state->flags & MB86H_DEVICE_STATE_CAPTURING) != streaming)
        return false;
    if (streaming && (!(state->flags & MB86H_DEVICE_STATE_STREAM_READY) ||
                      state->capture_generation != generation))
        return false;
    return !!(state->capture_mask & (1u << source)) == streaming;
}

int mb86H_session_finish_cleanup(struct mb86H_session *s,
                                    const struct mb86H_session_ops *ops,
                                    void *opaque, unsigned source,
                                    uint8_t response[64])
{
    uint32_t start;
    int rc;
    if (!polling_valid(s, ops) || !response) return -EINVAL;
    start = ops->now_ms(opaque);
    for (;;) {
        uint32_t status = mb86H_debug_u32be_get(response + 8);
        struct mb86H_source_state state;
        if (status != MB86H_DEBUG_OK && status != MB86H_DEBUG_FATAL)
            return -EPROTO;
        rc = queried_state(s, ops, opaque, source, &state);
        if (rc) return rc;
        if ((state.phase == MB86H_RESIDENT_PHASE_QUIET ||
             state.phase == MB86H_RESIDENT_PHASE_COMPLETE ||
             state.phase == MB86H_RESIDENT_PHASE_FAILED) &&
            !(state.flags & (MB86H_DEVICE_STATE_STARTED |
                             MB86H_DEVICE_STATE_OWNED |
                             MB86H_DEVICE_STATE_RESTORE_PENDING)))
            return 0;
        if ((uint32_t)(ops->now_ms(opaque) - start) > MB86H_INIT_TIMEOUT_MS)
            return -ETIMEDOUT;
        ops->delay_ms(opaque, 10);
        rc = mb86H_session_command(s, ops, opaque,
            MB86H_RESIDENT_COMMAND_SERVICE, 0, NULL, response);
        if (rc) return rc;
    }
}

static int session_settle(struct mb86H_session *s,
                           const struct mb86H_session_ops *ops, void *opaque,
                           unsigned source, uint32_t generation,
                           bool ready, bool streaming, bool *deadline_expired)
{
    uint8_t response[64];
    uint32_t start, timeout = s && s->tune_timeout_ms && generation ? s->tune_timeout_ms : MB86H_INIT_TIMEOUT_MS;
    int rc;
    if (!polling_valid(s, ops) || source >= MB86H_CHANNEL_COUNT)
        return -EINVAL;
    start = ops->now_ms(opaque);
    for (;;) {
        int32_t error;
        rc = mb86H_session_command(s, ops, opaque,
            MB86H_RESIDENT_COMMAND_SERVICE, source, NULL, response);
        if (rc) return rc;
        {
            struct mb86H_source_state state;
            error = queried_state(s, ops, opaque, source, &state);
            if (error < 0 || response_error(response)) {
                rc = mb86H_session_finish_cleanup(s, ops, opaque, source, response);
                return rc ? rc : error < 0 ? error : response_error(response);
            }
            if (query_complete(&state, source, generation, ready, streaming))
                return 0;
        }
        if ((uint32_t)(ops->now_ms(opaque) - start) > timeout) {
            if (deadline_expired) *deadline_expired = true;
            return -ETIMEDOUT;
        }
        /* Firmware gates every step on its hardware due time. Adapters may
         * select a finer foreground cadence for zero/one-ms calibration
         * transitions; other devices and initialization keep 10 ms. */
        ops->delay_ms(opaque, generation && s->tune_poll_ms ? s->tune_poll_ms : 10u);
    }
}

int mb86H_session_settle(struct mb86H_session *s,
                           const struct mb86H_session_ops *ops, void *opaque,
                           unsigned source, uint32_t generation, bool ready,
                           bool streaming)
{
    return session_settle(s, ops, opaque, source, generation, ready, streaming, NULL);
}

int mb86H_session_wait_stopped(struct mb86H_session *s,
                                 const struct mb86H_session_ops *ops, void *opaque)
{
    uint8_t response[64];
    uint32_t start;
    if (!polling_valid(s, ops)) return -EINVAL;
    start = ops->now_ms(opaque);
    while (s->stop_pending) {
        int rc = mb86H_session_command(s, ops, opaque,
            MB86H_RESIDENT_COMMAND_SERVICE, s->source, NULL, response);
        if (rc) return rc;
        {
            struct mb86H_source_state state;
            rc=response_error(response);
            if(rc)return rc;
            rc = queried_state(s, ops, opaque, s->source, &state);
            if (rc) return rc;
            if (state.generation == s->generation &&
                state.phase == MB86H_RESIDENT_PHASE_COMPLETE &&
                !(state.flags & MB86H_DEVICE_STATE_CAPTURING) &&
                !(state.capture_mask & (1u << s->source))) {
                s->stop_pending = false;
                break;
            }
        }
        if ((uint32_t)(ops->now_ms(opaque) - start) > MB86H_INIT_TIMEOUT_MS)
            return -ETIMEDOUT;
        ops->delay_ms(opaque, 10);
    }
    return 0;
}

int mb86H_session_prepare(struct mb86H_session *s,
    const struct mb86H_session_ops *ops, void *opaque,
    struct mb86H_device_overview *out)
{
    struct mb86H_device_overview overview;
    int rc;
    if (!s) return -EINVAL;
    rc = mb86H_device_query_overview(s, ops, opaque, &overview);
    if (rc) return rc;
    if (out) *out = overview;
    return 0;
}

int mb86H_session_initialize(struct mb86H_session *s,
    const struct mb86H_session_ops *ops, void *opaque)
{
    uint8_t response[64];
    int rc;
    if (!polling_valid(s, ops)) return -EINVAL;
    rc = mb86H_session_command(s, ops, opaque,
        MB86H_RESIDENT_COMMAND_INITIALIZE, 0, NULL, response);
    if (rc) return rc;
    rc = response_error(response);
    return rc ? rc : mb86H_session_settle(s, ops, opaque, 0, 0, false, false);
}

static int session_shutdown(struct mb86H_session *s,
    const struct mb86H_session_ops *ops, void *opaque, bool detach)
{
    uint8_t response[64];
    uint32_t start;
    int rc;
    if (!polling_valid(s, ops)) return -EINVAL;
    rc = mb86H_session_command(s, ops, opaque,
        MB86H_RESIDENT_COMMAND_SHUTDOWN, 0, NULL, response);
    if (rc) return rc;
    rc = response_error(response);
    if (rc) return rc;
    start = ops->now_ms(opaque);
    for (;;) {
        {
            struct mb86H_source_state state;
            rc = queried_shutdown_state(s, ops, opaque, &state);
            if (rc) return rc;
            if (shutdown_released(&state)) {
                /* Logical STOP can finish while an unreadable auxiliary
                 * preimage still requires physical recovery. Never turn
                 * that state into permission to overwrite/restore images. */
                if((state.flags&MB86H_DEVICE_STATE_COLD_REQUIRED) &&
                   (!detach || !(state.flags&MB86H_DEVICE_STATE_REINITIALIZABLE)))
                    return -EBUSY;
                s->request_valid = s->streaming = s->stop_pending = false;
                return 0;
            }
        }
        if (!response_ok(response)) return response_error(response);
        if ((uint32_t)(ops->now_ms(opaque) - start) > MB86H_INIT_TIMEOUT_MS)
            return -ETIMEDOUT;
        ops->delay_ms(opaque, 10);
        rc = mb86H_session_command(s, ops, opaque,
            MB86H_RESIDENT_COMMAND_SERVICE, 0, NULL, response);
        if (rc) return rc;
        rc = response_error(response);
        if (rc) return rc;
    }
}

int mb86H_session_shutdown(struct mb86H_session *s,
    const struct mb86H_session_ops *ops, void *opaque)
{
    return session_shutdown(s,ops,opaque,false);
}

int mb86H_session_detach(struct mb86H_session *s,
    const struct mb86H_session_ops *ops, void *opaque)
{
    return session_shutdown(s,ops,opaque,true);
}

int mb86H_session_tune(struct mb86H_session *s,
                         const struct mb86H_session_ops *ops, void *opaque,
                         unsigned source, const struct mb86H_dvb_request *request,
                         bool streaming)
{
    uint8_t data[40] = {0}, response[64];
    uint32_t frequency, generation;
    bool deadline_expired = false;
    int rc;
    if (!polling_valid(s, ops) || !request || source >= MB86H_CHANNEL_COUNT)
        return -EINVAL;
    rc = mb86H_session_wait_stopped(s, ops, opaque);
    if (rc) return rc;
    frequency = source ? request->frequency + 10678000u :
                         (request->frequency + 500u) / 1000u;
    mb86H_debug_u32be_put(data, frequency);
    data[4] = request->stream_id >> 8;
    data[5] = request->stream_id;
    data[6] = source ? 0 : ((request->layers & 1u) << 7) |
                          ((request->layers & 2u) << 5) | ((request->layers & 4u) << 3);
    data[7] = streaming;
    data[8] = request->stream_id != (uint32_t)-1;
    s->request_valid = false;
    s->streaming = false;
    rc = mb86H_session_command(s, ops, opaque,
        MB86H_RESIDENT_COMMAND_TUNE, source, data, response);
    if (rc || response_error(response)) return rc ? rc : response_error(response);
    generation = mb86H_debug_u32be_get(response + 16);
    if (!generation) return -EIO;
    rc = session_settle(s, ops, opaque, source, generation, true, streaming,
                        &deadline_expired);
    if (deadline_expired) {
        /* TUNE was acknowledged: a local lock deadline does not retire its
         * firmware generation. Cancel once before admitting another tune. */
        int stopped;
        s->request = *request;
        mb86H_session_begin(s, source, generation);
        stopped = mb86H_session_stop(s, ops, opaque, source);
        if (!stopped) stopped = mb86H_session_wait_stopped(s, ops, opaque);
        if (stopped) return stopped;
        s->request_valid = false;
    }
    if (!rc) {
        s->request = *request;
        mb86H_session_begin(s, source, generation);
        s->streaming = streaming;
    }
    return rc;
}

int mb86H_session_stop(struct mb86H_session *s,
                         const struct mb86H_session_ops *ops, void *opaque,
                         unsigned source)
{
    uint8_t response[64];
    uint32_t generation;
    int rc;
    if (!polling_valid(s, ops) || !mb86H_session_matches(s, source))
        return -EINVAL;
    s->streaming = false;
    if (s->stop_pending) return 0;
    rc = mb86H_session_command(s, ops, opaque,
        MB86H_RESIDENT_COMMAND_STOP, source, NULL, response);
    if (rc || response_error(response)) {
        s->request_valid = false;
        return rc ? rc : response_error(response);
    }
    generation = mb86H_debug_u32be_get(response + 16);
    if (!generation) { s->request_valid = false; return -EIO; }
    /* STOP acknowledges ownership of cleanup; firmware advances QUIESCE
     * independently. Do not poll SERVICE or wait for RF release here. */
    s->generation = generation;
    s->stop_pending = true;
    return 0;
}

int mb86H_session_service(struct mb86H_session *s,
                            const struct mb86H_session_ops *ops, void *opaque)
{
    uint8_t response[64];
    int rc;
    if (!polling_valid(s, ops) || !s->request_valid) return -EINVAL;
    rc = mb86H_session_command(s, ops, opaque,
        MB86H_RESIDENT_COMMAND_SERVICE, s->source, NULL, response);
    if (rc) return rc;
    return response_error(response);
}

int mb86H_session_locked(struct mb86H_session *s,
                           const struct mb86H_session_ops *ops, void *opaque,
                           unsigned source, bool *locked)
{
    uint8_t response[64];
    struct mb86H_source_state state;
    int rc;
    if (!locked) return -EINVAL;
    *locked = false;
    if (!mb86H_session_matches(s, source)) return -EINVAL;
    rc = mb86H_session_command(s, ops, opaque,
        MB86H_RESIDENT_COMMAND_STATUS, source, NULL, response);
    if (rc || response_error(response)) return rc ? rc : response_error(response);
    rc = queried_state(s, ops, opaque, source, &state);
    if (rc) return rc;
    *locked = state.generation == s->generation &&
        state.phase == MB86H_RESIDENT_PHASE_COMPLETE &&
        !!(state.flags & MB86H_DEVICE_STATE_READY);
    return 0;
}

int mb86H_session_stream(struct mb86H_session *s,
                           const struct mb86H_session_ops *ops, void *opaque,
                           unsigned source, bool enable)
{
    struct mb86H_dvb_request request;
    if (!mb86H_session_matches(s, source)) return -EINVAL;
    if (!enable) return mb86H_session_stop(s, ops, opaque, source);
    request = s->request;
    return mb86H_session_tune(s, ops, opaque, source, &request, true);
}
