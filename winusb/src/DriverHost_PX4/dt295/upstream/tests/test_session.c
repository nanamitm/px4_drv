#include "mb86H/session.h"
#include "mb86H/device_query.h"
#include "mb86H/protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    unsigned calls, services, stops;
    uint32_t now, generation, source, ready_after;
    int failure, firmware_error, service_failure;
    bool ready, streaming, stale, zero_generation, initial_quiet, cancelled;
    bool defer_stop, shutdown_pending, shutdown_complete, bad_window, window_wait;
    int query_error, shutdown_historical_error;
    bool cold_required,reinitializable;
    unsigned stop_statuses;
    uint8_t tune[40];
};
static uint32_t now(void *opaque) { return ((struct fixture *)opaque)->now; }
static void delay(void *opaque, unsigned ms) { ((struct fixture *)opaque)->now += ms; }
static int exchange(void *opaque, const uint8_t request[64], uint8_t response[64])
{
    struct fixture *f = opaque;
    uint32_t cmd = mb86H_debug_u32be_get(request + 12);
    ++f->calls;
    if (f->failure) return f->failure;
    if (cmd == MB86H_RESIDENT_COMMAND_SERVICE && f->service_failure)
        return f->service_failure;
    memset(response, 0, 64);
    memcpy(response, request, 8);
    if (cmd == MB86H_RESIDENT_COMMAND_DEVICE_QUERY) {
        unsigned page = mb86H_debug_u32be_get(request + 16);
        uint8_t *data = response + 24;
        bool stopping = f->defer_stop && !f->ready && f->stop_statuses < 3;
        bool acquiring = !f->cancelled && f->ready_after && f->now < f->ready_after;
        struct mb86H_source_state state = {
            .generation = f->stale ? f->generation - 1 : f->generation,
            .error = (f->shutdown_pending || f->shutdown_complete) ?
                f->shutdown_historical_error : f->query_error,
            .phase = f->initial_quiet && !f->generation ? MB86H_RESIDENT_PHASE_QUIET :
                stopping ? MB86H_RESIDENT_PHASE_QUIESCE : acquiring ?
                MB86H_RESIDENT_PHASE_ACQUIRE :
                MB86H_RESIDENT_PHASE_COMPLETE,
            .flags = f->shutdown_complete ? 0 :
                MB86H_DEVICE_STATE_STARTED | MB86H_DEVICE_STATE_INITIALIZED |
                (f->ready && !acquiring ? MB86H_DEVICE_STATE_READY : 0) |
                ((f->streaming || stopping) ? MB86H_DEVICE_STATE_CAPTURING : 0) |
                (f->streaming ? MB86H_DEVICE_STATE_STREAM_READY : 0),
            .capture_generation = (f->streaming || stopping) ? f->generation : 0,
            .capture_mask = (f->streaming || stopping) ? 1u << f->source : 0,
        };
        if(f->cold_required)state.flags|=MB86H_DEVICE_STATE_COLD_REQUIRED;
        if(f->reinitializable)state.flags|=MB86H_DEVICE_STATE_REINITIALIZABLE;
        mb86H_debug_u32be_put(response + 12, MB86H_RESIDENT_COMMAND_DEVICE_QUERY);
        mb86H_debug_u32be_put(response + 16, page);
        data[0] = MB86H_DEVICE_QUERY_VERSION;
        data[1] = page;
        data[2] = request[25];
        if (page == MB86H_DEVICE_QUERY_OVERVIEW) {
            struct mb86H_device_overview overview = {
                .rf_mask = 3, .stream_mask = 3,
                .resident_abi = MB86H_RESIDENT_ABI_VERSION,
                .window_version = MB86H_STREAM_WINDOW_VERSION,
            };
            mb86H_query_overview_encode(data, &overview);
        } else if (page == MB86H_DEVICE_QUERY_STATE)
            mb86H_query_state_encode(data, &state);
        return 0;
    }
    if (cmd == MB86H_RESIDENT_COMMAND_STREAM_WINDOW) {
        uint8_t *data = response + 24;
        mb86H_debug_u32be_put(response + 8, MB86H_DEBUG_OK);
        mb86H_debug_u32be_put(response + 12, 1);
        mb86H_debug_u32be_put(response + 16,
                               mb86H_debug_u32be_get(request + 32));
        data[0] = MB86H_STREAM_WINDOW_VERSION;
        data[1] = 1;
        data[2] = 1;
        if (f->window_wait)
            mb86H_debug_u32be_put(response + 20, 1);
        mb86H_debug_u32be_put(data + 4,
                               mb86H_debug_u32be_get(request + 28));
        mb86H_debug_u32be_put(data + 8,
                               mb86H_debug_u32be_get(request + 32));
        mb86H_debug_u32be_put(data + 28,
                               mb86H_debug_u32be_get(request + 36));
        if (f->bad_window)
            mb86H_debug_u32be_put(response + 12, 0);
        return 0;
    }
    if (cmd == MB86H_RESIDENT_COMMAND_TUNE || cmd == MB86H_RESIDENT_COMMAND_STOP) {
        f->cancelled = cmd == MB86H_RESIDENT_COMMAND_STOP;
        if (f->cancelled) ++f->stops;
        f->source = mb86H_debug_u32be_get(request + 16);
        ++f->generation;
        f->ready = cmd == MB86H_RESIDENT_COMMAND_TUNE;
        f->streaming = f->ready && request[31];
        memcpy(f->tune, request + 24, 40);
        mb86H_debug_u32be_put(response + 16, f->zero_generation ? 0 : f->generation);
    } else if (cmd == MB86H_RESIDENT_COMMAND_SERVICE) {
        ++f->services;
        if (f->defer_stop && !f->ready)
            ++f->stop_statuses;
        if (f->shutdown_pending) {
            f->shutdown_pending = false;
            f->shutdown_complete = true;
            f->ready = f->streaming = false;
        }
    } else if (cmd == MB86H_RESIDENT_COMMAND_SHUTDOWN) {
        f->shutdown_pending = true;
    }
    response[26] = 1;
    response[24 + f->source] = MB86H_RESIDENT_PHASE_COMPLETE;
    mb86H_debug_u32be_put(response + 28 + f->source * 4,
                             f->stale ? f->generation - 1 : f->generation);
    mb86H_debug_u32be_put(response + 12, f->ready ? 1u << f->source : 0);
    response[46] = f->streaming ? 1u << f->source : 0;
    if (f->defer_stop && !f->ready) {
        if (cmd == MB86H_RESIDENT_COMMAND_STATUS) ++f->stop_statuses;
        if (f->stop_statuses < 3) {
            response[24 + f->source] = MB86H_RESIDENT_PHASE_QUIESCE;
            response[46] = 1u << f->source;
        }
    }
    if (!f->cancelled && f->ready_after && f->now < f->ready_after) {
        response[24] = MB86H_RESIDENT_PHASE_ACQUIRE;
        mb86H_debug_u32be_put(response + 20, 1);
        mb86H_debug_u32be_put(response + 40, 100);
    }
    if (f->firmware_error) {
        mb86H_debug_u32be_put(response + 8, MB86H_DEBUG_FATAL);
        mb86H_debug_u32be_put(response + 20, (uint32_t)f->firmware_error);
    }
    return 0;
}
static const struct mb86H_session_ops ops = {
    .exchange = exchange, .now_ms = now, .delay_ms = delay,
};
int main(void)
{
    struct mb86H_session s = {0};
    struct fixture f = {0};
    struct mb86H_dvb_request gr = {
        .system = MB86H_DVB_GR, .frequency = 539143000,
        .stream_id = (uint32_t)-1, .layers = 7,
    };
    uint8_t response[64];
    uint32_t sequence;
    bool locked;
    struct mb86H_device_overview overview;
    /* A hardware due time between 10 ms ticks must remain authoritative,
     * without adding a full tick to foreground acquisition. */
    f.ready_after = 25;
    s.tune_poll_ms = 1;
    assert(!mb86H_session_initialize(&s, &ops, &f));
    assert(f.now == 30);
    memset(&f, 0, sizeof(f));
    f.ready_after = 25;
    assert(!mb86H_session_tune(&s, &ops, &f, 0, &gr, false));
    assert(f.now == 25);
    memset(&f, 0, sizeof(f));
    mb86H_session_reset(&s);
    f.ready_after = 25;
    assert(!mb86H_session_tune(&s, &ops, &f, 0, &gr, false));
    assert(f.now == 30); /* default devices retain their established cadence */
    memset(&f, 0, sizeof(f));
    mb86H_session_reset(&s);
    assert(!mb86H_session_prepare(&s, &ops, &f, &overview));
    assert(overview.rf_mask == 3 && overview.stream_mask == 3);
    assert(!mb86H_session_tune(&s, &ops, &f, 0, &gr, false));
    assert(s.request_valid && s.generation == 1 && !s.streaming);
    assert(mb86H_debug_u32be_get(f.tune) == 539143);
    assert(f.tune[6] == 0xe0 && !f.tune[7] && !f.tune[8]);
    assert(!mb86H_session_locked(&s, &ops, &f, 0, &locked) && locked);
    sequence = s.sequence;
    assert(!mb86H_session_stream(&s, &ops, &f, 0, true));
    assert(s.streaming && s.sequence > sequence && f.tune[6] == 0xe0 && f.tune[7]);
    f.defer_stop = true;
    {
        unsigned calls = f.calls, services = f.services;
        assert(!mb86H_session_stream(&s, &ops, &f, 0, false));
        assert(f.calls == calls + 1 && f.services == services && !f.stop_statuses);
        assert(s.stop_pending && !s.streaming && !f.ready && !f.streaming);
        assert(!mb86H_session_stream(&s, &ops, &f, 0, false));
        assert(f.calls == calls + 1); /* no duplicate STOP */
    }
    assert(!mb86H_session_stream(&s, &ops, &f, 0, true));
    assert(f.stop_statuses == 3 && !s.stop_pending);
    f.defer_stop = false;
    f.failure = -EPIPE;
    {
        unsigned calls = f.calls;
        assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -EPIPE);
        assert(f.calls == calls + 1 && !s.request_valid);
    }
    f.failure = 0; f.zero_generation = true;
    assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -EIO);
    assert(!s.request_valid);
    f.zero_generation = false; f.stale = true;
    assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -ETIMEDOUT);
    assert(s.request_valid && s.stop_pending && f.stops == 2);
    f.stale = false;
    mb86H_session_reset(&s);
    memset(&f, 0, sizeof(f));
    s.tune_timeout_ms = 20;
    f.ready_after = 1000;
    assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -ETIMEDOUT);
    assert(f.stops == 1 && !s.request_valid && !s.stop_pending && !f.ready);
    f.ready_after = 0;
    assert(!mb86H_session_tune(&s, &ops, &f, 0, &gr, false));
    mb86H_session_reset(&s);
    memset(&f, 0, sizeof(f));
    f.service_failure = -ETIMEDOUT;
    assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -ETIMEDOUT);
    assert(!f.stops); /* Ambiguous transport timeout is never replayed/cancelled. */
    mb86H_session_reset(&s);
    memset(&f, 0, sizeof(f));
    f.firmware_error = -2603;
    assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -2603);
    f.firmware_error = 0;
    mb86H_session_reset(&s);
    memset(&f, 0, sizeof(f));
    assert(!mb86H_session_prepare(&s, &ops, &f, &overview));
    f.firmware_error = -2603;
    assert(mb86H_session_tune(&s, &ops, &f, 0, &gr, false) == -2603);
    f.firmware_error = 0;
    mb86H_session_reset(&s);
    memset(&f, 0, sizeof(f));
    assert(!mb86H_session_prepare(&s, &ops, &f, &overview));
    f.initial_quiet = true;
    assert(!mb86H_session_initialize(&s, &ops, &f));
    assert(f.now == 0 && f.services == 1);
    assert(!mb86H_session_tune(&s, &ops, &f, 0, &gr, true));
    assert(!mb86H_session_locked(&s, &ops, &f, 0, &locked) && locked);
    assert(!mb86H_device_stream_window(&s, &ops, &f, 0, s.generation, 7, 20));
    f.window_wait = true;
    assert(!mb86H_device_stream_window(&s, &ops, &f, 0, s.generation, 8, 20));
    f.window_wait = false;
    f.bad_window = true;
    assert(mb86H_device_stream_window(&s, &ops, &f, 0, s.generation, 8, 20) == -EPROTO);
    f.bad_window = false;
    f.query_error = -2603;
    assert(mb86H_session_locked(&s, &ops, &f, 0, &locked) == -2603);
    f.query_error = 0;
    f.defer_stop = true;
    assert(!mb86H_session_stop(&s, &ops, &f, 0));
    assert(!mb86H_session_wait_stopped(&s, &ops, &f));
    assert(f.stop_statuses == 3 && !s.stop_pending);
    f.defer_stop = false;
    f.shutdown_historical_error = -2603;
    assert(!mb86H_session_shutdown(&s, &ops, &f));
    assert(f.shutdown_complete && !s.request_valid);
    f.cold_required=true;f.shutdown_complete=f.shutdown_pending=false;
    s.request_valid=true;
    unsigned services_before=f.services;
    assert(mb86H_session_shutdown(&s,&ops,&f)==-EBUSY);
    assert(f.shutdown_complete && s.request_valid && f.services==services_before+1);
    f.shutdown_complete=f.shutdown_pending=false;
    assert(mb86H_session_detach(&s,&ops,&f)==-EBUSY && s.request_valid);
    f.reinitializable=true;f.shutdown_complete=f.shutdown_pending=false;
    assert(mb86H_session_shutdown(&s,&ops,&f)==-EBUSY && s.request_valid);
    f.shutdown_complete=f.shutdown_pending=false;
    assert(!mb86H_session_detach(&s,&ops,&f) && !s.request_valid);
    f.cold_required=false;
    /* Auxiliary command selectors are not limited to RF source indices. */
    assert(!mb86H_session_command(&s, &ops, &f,
        MB86H_RESIDENT_COMMAND_AUX_IMAGES, 4, NULL, response));
    assert(mb86H_session_command(&s, NULL, &f, 0, 0, NULL, response) == -EINVAL);
    {
        struct mb86H_session_ops invalid = {.exchange = exchange};
        unsigned calls = f.calls;
        assert(mb86H_session_tune(&s, &invalid, &f, 0, &gr, false) == -EINVAL);
        assert(f.calls == calls);
    }
    /* Sony RF-only uses the same session generation/error handling with
     * the actual Sony TUNE payload; TS rejection performs no exchange. */
    mb86H_session_reset(&s);
    s.tune_timeout_ms = 70000;
    memset(&f, 0, sizeof(f));
    f.ready_after = 20000;
    assert(!mb86H_session_tune(&s, &ops, &f, 0, &gr, false));
    assert(f.now == 20000);
    assert(mb86H_debug_u32be_get(f.tune) == 539143);
    assert(f.tune[6] == 0xe0 && !f.tune[7] && !f.tune[8]);
    assert(!mb86H_session_locked(&s, &ops, &f, 0, &locked) && locked);
    puts("session lifecycle tests passed");
    return 0;
}
