#include "mb86H/loader.h"
#include "mb86H/device.h"
#include "mb86H/device_query.h"
#include "mb86H/protocol.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); return 1; \
} } while (0)

struct mock {
    unsigned char boot[0x100000];
    unsigned char resident[MB86H_RESIDENT_BYTES];
    unsigned char torne_ts_ring[MB86H_TORNE_TS_RING_BYTES];
    unsigned char execution[4];
    unsigned writes;
    unsigned ep0_writes[2];
    unsigned dispatches;
    unsigned block_writes[2];
    unsigned snapshots;
    unsigned image_writes, window_writes, recover_calls, initialize_calls,releases;
    unsigned resident_block_reads, mismatch_count;
    uint32_t mismatch_address, mismatch_expected, mismatch_observed;
    unsigned oversized_window_reads;
    const unsigned char *expected_hook;
    bool bad_capability_build;
    int recover_error;
    unsigned capability_mask;
    uint32_t ticks;
    bool initialized;
    unsigned shutdown_calls, shutdown_services;
    bool shutdown_pending;
    bool cold_required,reinitializable;
    bool mutate_staging_on_resident_read;
    int fail_after;
};

static unsigned be32(const unsigned char *p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static int window_read(void *opaque, uint32_t address, void *data, uint16_t length)
{
    struct mock *m = opaque;
    CHECK(length != 0);
    /* Native XZ boot0 requests are bounded to the existing 64-byte exchange;
     * reading its entire 88-byte hook in one request timed out before seq 1. */
    if (m->capability_mask == 3 && length > 64) {
        m->oversized_window_reads++;
        return -110;
    }
    /* Published XZ replaces the native copy loop: recovery must use its packet
     * gateway, not attempt native hook/body inspection through that same IN. */
    if (m->capability_mask == 3 && m->expected_hook &&
        !memcmp(m->boot + MB86H_BOOT0_HOOK_ADDRESS, m->expected_hook, 88))
        return -71;
    if (address >= MB86H_RESIDENT_BASE &&
        address - MB86H_RESIDENT_BASE <= MB86H_RESIDENT_BYTES - length) {
        memcpy(data, m->resident + address - MB86H_RESIDENT_BASE, length);
        if (m->mutate_staging_on_resident_read)
            memset(m->boot + MB86H_BOOT0_STAGING_ADDRESS, 0, 64);
    } else if (address >= MB86H_TORNE_TS_RING_BASE &&
               address - MB86H_TORNE_TS_RING_BASE <=
                   MB86H_TORNE_TS_RING_BYTES - length) {
        memcpy(data, m->torne_ts_ring + address - MB86H_TORNE_TS_RING_BASE, length);
    } else if (address == 0xfd0090 && length == sizeof(m->execution))
        memcpy(data, m->execution, length);
    else if (address <= sizeof(m->boot) - length)
        memcpy(data, m->boot + address, length);
    else return -1;
    return 0;
}

static int window_write(void *opaque, uint32_t address, const void *data, uint16_t length)
{
    struct mock *m = opaque;
    m->window_writes++;
    if (m->fail_after >= 0 && (int)m->writes++ == m->fail_after) return -5;
    if (address >= MB86H_RESIDENT_BASE &&
        address - MB86H_RESIDENT_BASE <= MB86H_RESIDENT_BYTES - length)
        memcpy(m->resident + address - MB86H_RESIDENT_BASE, data, length);
    else if (address >= MB86H_TORNE_TS_RING_BASE &&
             address - MB86H_TORNE_TS_RING_BASE <=
                 MB86H_TORNE_TS_RING_BYTES - length)
        memcpy(m->torne_ts_ring + address - MB86H_TORNE_TS_RING_BASE, data, length);
    else {
        if (address == 0xe9b54 || address == 0xe9b58) { CHECK(length == 4); m->ep0_writes[0]++; }
        if (address == 0xe9c08 || address == 0xe9c0c) { CHECK(length == 4); m->ep0_writes[1]++; }
        if (address <= sizeof(m->boot) - length) memcpy(m->boot + address, data, length);
        else return -1;
    }
    return 0;
}

static int exchange(void *opaque, const uint8_t request[64], uint8_t response[64])
{
    struct mock *m = opaque;
    unsigned op = be32(request + 8), address = be32(request + 12);
    unsigned argument = be32(request + 16), budget = be32(request + 20);
    memset(response, 0, 64);
    memcpy(response, request, 8);
    if (op == MB86H_DEBUG_PING) {
        unsigned address = m->capability_mask == 3 ? MB86H_BOOT0_HOOK_ADDRESS :
                                                    MB86H_TORNE_BOOT0_HOOK_ADDRESS;
        size_t hook_size = m->capability_mask == 3 ? 88 : 8;
        if (m->expected_hook && memcmp(m->boot + address, m->expected_hook, hook_size)) {
            memcpy(response, request, 64); /* Factory native copy, not a gateway. */
            return 0;
        }
        mb86H_debug_u32be_put(response + 8, MB86H_DEBUG_OK);
        mb86H_debug_u32be_put(response + 12, 1);
        return 0;
    }
    if (op == MB86H_DEBUG_RESIDENT_BLOCK) {
        CHECK(address <= MB86H_RESIDENT_BYTES - budget);
        if (argument) { m->image_writes++; memcpy(m->resident + address, request + 24, budget); }
        else { m->resident_block_reads++; memcpy(response + 24, m->resident + address, budget); }
        mb86H_debug_u32be_put(response + 12, budget);
        mb86H_debug_u32be_put(response + 16, address);
        return 0;
    }
    CHECK(op == MB86H_DEBUG_RESIDENT_DISPATCH);
    m->dispatches++;
    mb86H_debug_u32be_put(response + 8, MB86H_DEBUG_OK);
    if (address == MB86H_RESIDENT_COMMAND_DEVICE_QUERY) {
        uint8_t *data=response+24;
        unsigned page=argument;
        mb86H_debug_u32be_put(response + 12, MB86H_RESIDENT_COMMAND_DEVICE_QUERY);
        mb86H_debug_u32be_put(response + 16, page);
        data[0]=MB86H_DEVICE_QUERY_VERSION;
        data[1]=(uint8_t)page;
        data[2]=request[25];
        if(page==MB86H_DEVICE_QUERY_OVERVIEW) {
            struct mb86H_device_overview overview={
                .rf_mask=m->capability_mask,.stream_mask=m->capability_mask,
                .resident_abi=MB86H_RESIDENT_ABI_VERSION,
            };
            mb86H_query_overview_encode(data,&overview);
        } else if(page==MB86H_DEVICE_QUERY_STATE) {
            struct mb86H_source_state state={
                .phase=MB86H_RESIDENT_PHASE_COMPLETE,
                .flags=m->initialized ? MB86H_DEVICE_STATE_STARTED |
                    MB86H_DEVICE_STATE_INITIALIZED : 0,
            };
            if(m->cold_required)state.flags|=MB86H_DEVICE_STATE_COLD_REQUIRED;
            if(m->reinitializable)state.flags|=MB86H_DEVICE_STATE_REINITIALIZABLE;
            mb86H_query_state_encode(data,&state);
        }
    } else if (address == MB86H_RESIDENT_COMMAND_CAPABILITIES) {
        mb86H_debug_u32be_put(response + 16, MB86H_RESIDENT_ABI_VERSION);
        memcpy(response + 24, MB86H_FIRMWARE_IDENTITY,
               MB86H_FIRMWARE_IDENTITY_BYTES);
        memcpy(response + MB86H_DEBUG_PACKET_DATA_OFFSET + 16,
               m->resident + MB86H_RESIDENT_BUILD_ID - MB86H_RESIDENT_BASE, 8);
        memcpy(response + MB86H_DEBUG_PACKET_DATA_OFFSET + 24,
               m->resident + MB86H_RESIDENT_BUILD_ID - MB86H_RESIDENT_BASE, 8);
        if (m->bad_capability_build)
            response[MB86H_DEBUG_PACKET_DATA_OFFSET + 16] ^= 1;
        response[MB86H_DEBUG_PACKET_DATA_OFFSET + 15] = m->capability_mask;
    } else if (address == MB86H_RESIDENT_COMMAND_INITIALIZE) {
        m->initialize_calls++;
        m->initialized = true;
    } else if (address == MB86H_RESIDENT_COMMAND_RECOVER) {
        m->recover_calls++;
        if (m->recover_error) return m->recover_error;
        response[26] = response[62] = response[63] = 0xa5;
        if (!(m->recover_calls % 3)) m->initialized = false;
    } else if (address == MB86H_RESIDENT_COMMAND_SHUTDOWN) {
        ++m->shutdown_calls;
        if (m->capability_mask == 1 && m->initialized) {
            if (!m->shutdown_pending) m->shutdown_pending = true;
            if (++m->shutdown_services < 3) response[63] = 1;
            else { m->shutdown_pending = false; m->initialized = false; }
        } else m->initialized = false;
    } else if (address == MB86H_RESIDENT_COMMAND_SERVICE) {
        if (m->shutdown_pending) {
            if (++m->shutdown_services < 3) response[63] = 1;
            else { m->shutdown_pending = false; m->initialized = false; }
        }
        response[24] = MB86H_RESIDENT_PHASE_COMPLETE;
        response[26] = m->initialized;
    } else if (address == MB86H_RESIDENT_COMMAND_STATUS) {
        response[26] = m->initialized;
    }
    return 0;
}

static uint32_t now_ms(void *opaque) { return ((struct mock *)opaque)->ticks; }
static void delay_ms(void *opaque, unsigned ms) { ((struct mock *)opaque)->ticks += ms; }
static void *allocate(void *opaque, size_t size) { (void)opaque; return malloc(size); }
static void release(void *opaque, void *p)
{struct mock *m=opaque;++m->releases;free(p);}

static int block_writes(void *opaque, bool enable)
{
    struct mock *m = opaque;
    m->block_writes[enable ? 1 : 0]++;
    return 0;
}

static int snapshot(void *opaque, const struct mb86H_loader *loader)
{
    struct mock *m = opaque;
    if (!loader->resident_saved ||
        (loader->is_torne && (!loader->torne_boot0_saved ||
                              memcmp(loader->torne_out_original,
                                     "\xb4\x27\x9f\x8d", 4))))
        return -1;
    m->snapshots++;
    return 0;
}

static void mismatch(void *opaque, uint32_t address, uint32_t expected, uint32_t observed)
{
    struct mock *m = opaque;
    m->mismatch_count++;
    m->mismatch_address = address;
    m->mismatch_expected = expected;
    m->mismatch_observed = observed;
    fprintf(stderr, "mismatch %x expected=%x observed=%x\n", address, expected, observed);
}

static void make_resident_image(unsigned char *image)
{
    uint32_t offset, sum = 0;
    unsigned char *meta = image + MB86H_RESIDENT_META - MB86H_RESIDENT_BASE;

    mb86H_debug_u32be_put(meta, MB86H_RESIDENT_HEADER_MAGIC);
    mb86H_debug_u32be_put(meta + 4, MB86H_RESIDENT_ABI_VERSION);
    mb86H_debug_u32be_put(meta + 8, MB86H_RESIDENT_BYTES);
    mb86H_debug_u32be_put(meta + 12, MB86H_RESIDENT_BASE);
    mb86H_debug_u32be_put(meta + 16, MB86H_RESIDENT_IMMUTABLE_END);
    mb86H_debug_u32be_put(meta + 20, MB86H_RESIDENT_IMMUTABLE_CHECKSUM);
    mb86H_debug_u32be_put(image + MB86H_RESIDENT_COMMIT - MB86H_RESIDENT_BASE,
                             MB86H_RESIDENT_COMMIT_MAGIC);
    mb86H_debug_u32be_put(image + MB86H_RESIDENT_IMMUTABLE_CHECKSUM -
                             MB86H_RESIDENT_BASE, 0);
    for (offset = 0; offset < MB86H_RESIDENT_IMMUTABLE_END - MB86H_RESIDENT_BASE;
         offset += 4)
        sum += be32(image + offset);
    mb86H_debug_u32be_put(image + MB86H_RESIDENT_IMMUTABLE_CHECKSUM -
                             MB86H_RESIDENT_BASE, 0u - sum);
}

static int test_torne(bool initialize)
{
    struct mock m = { .fail_after = -1, .capability_mask = 1,
                      .mutate_staging_on_resident_read = true };
    struct mb86H_session session;
    struct mb86H_session_ops session_ops = { exchange, NULL, now_ms, delay_ms };
    struct mb86H_loader_ops ops = {
        .window_read = window_read, .window_write = window_write,
        .session = &session, .session_ops = &session_ops, .now_ms = now_ms,
        .delay_ms = delay_ms, .allocate = allocate, .release = release,
        .block_writes = block_writes, .snapshot = snapshot, .mismatch = mismatch,
    };
    struct mb86H_loader loader;
    size_t root_boot_size = MB86H_BOOT0_BODY_OFFSET + sizeof(loader.body);
    size_t original = 64, image = original + root_boot_size;
    size_t resident = image + root_boot_size;
    size_t descriptor = resident + MB86H_RESIDENT_BYTES;
    size_t sony_original = descriptor + 64, sony_image = sony_original + 0x10000;
    size_t total = sony_image + 0x10000;
    unsigned char *bundle = calloc(1, total);
    unsigned char staging[64];
    unsigned i;

    CHECK(bundle);
    memcpy(bundle, "XZFB", 4); mb86H_debug_u32be_put(bundle + 4, 1);
    mb86H_debug_u32be_put(bundle + 8, 64); mb86H_debug_u32be_put(bundle + 12, MB86H_BOOT0_BASE);
    mb86H_debug_u32be_put(bundle + 16, root_boot_size); mb86H_debug_u32be_put(bundle + 20, original);
    mb86H_debug_u32be_put(bundle + 24, image); mb86H_debug_u32be_put(bundle + 28, MB86H_RESIDENT_BASE);
    mb86H_debug_u32be_put(bundle + 32, MB86H_RESIDENT_BYTES); mb86H_debug_u32be_put(bundle + 36, resident);
    mb86H_debug_u32be_put(bundle + 48, total);
    mb86H_debug_u32be_put(bundle + 52, 0x544f524e); mb86H_debug_u32be_put(bundle + 56, descriptor);
    mb86H_debug_u32be_put(bundle + 60, 1);
    memcpy(bundle + descriptor, "TNFB", 4); mb86H_debug_u32be_put(bundle + descriptor + 4, 1);
    mb86H_debug_u32be_put(bundle + descriptor + 8, 64); mb86H_debug_u32be_put(bundle + descriptor + 12, MB86H_BOOT0_BASE);
    mb86H_debug_u32be_put(bundle + descriptor + 16, 0x10000); mb86H_debug_u32be_put(bundle + descriptor + 20, sony_original);
    mb86H_debug_u32be_put(bundle + descriptor + 24, sony_image); mb86H_debug_u32be_put(bundle + descriptor + 28, MB86H_RESIDENT_BASE);
    mb86H_debug_u32be_put(bundle + descriptor + 32, MB86H_RESIDENT_BYTES); mb86H_debug_u32be_put(bundle + descriptor + 36, resident);
    mb86H_debug_u32be_put(bundle + descriptor + 48, total);
    for (i = 0; i < 0x10000; ++i) bundle[sony_original + i] = (unsigned char)(i * 13u);
    memcpy(bundle + sony_image, bundle + sony_original, 0x10000);
    memset(bundle + sony_image + MB86H_TORNE_BOOT0_BODY_ADDRESS - MB86H_BOOT0_BASE,
           0xa5, MB86H_TORNE_BOOT0_BODY_BYTES);
    memcpy(bundle + sony_image + MB86H_TORNE_BOOT0_HOOK_ADDRESS - MB86H_BOOT0_BASE,
           "\x9b\xec\x10\0\x97\x0c\x9f\xa0", 8);
    m.expected_hook = bundle + sony_image + MB86H_TORNE_BOOT0_HOOK_ADDRESS - MB86H_BOOT0_BASE;
    memcpy(bundle + sony_original + MB86H_TORNE_BOOT0_OUT_PATCH_ADDRESS - MB86H_BOOT0_BASE,
           "\xb4\x27\x9f\x8d", 4);
    make_resident_image(bundle + resident);
    memcpy(m.boot + MB86H_BOOT0_BASE, bundle + sony_original, 0x10000);
    memset(m.boot + MB86H_TORNE_BOOT0_BODY_ADDRESS, 0x3c,
           MB86H_TORNE_BOOT0_BODY_BYTES);
    for (i = 0; i < MB86H_RESIDENT_BYTES; ++i) m.resident[i] = (unsigned char)(i * 11u);
    for (i = 0; i < MB86H_TORNE_TS_RING_BYTES; ++i)
        m.torne_ts_ring[i] = (unsigned char)(i * 17u);
    for (i = 0; i < sizeof(staging); ++i) staging[i] = (unsigned char)(0x80u + i);
    memcpy(m.boot + MB86H_BOOT0_STAGING_ADDRESS, staging, sizeof(staging));
    mb86H_session_reset(&session);
    CHECK(!mb86H_loader_parse(&loader, bundle, total));
    CHECK(!mb86H_loader_parse_target(&loader, bundle, total, true));
    CHECK(!mb86H_device_firmware_parse(mb86H_torne_device(), &loader, bundle, total));
    CHECK(loader.is_torne);
    CHECK(!(initialize ? mb86H_loader_start(&loader, &ops, &m) :
                        mb86H_loader_install(&loader, &ops, &m)));
    CHECK(m.snapshots == 1 && m.block_writes[1] == 1);
    CHECK(!memcmp(m.boot + MB86H_TORNE_BOOT0_HOOK_ADDRESS,
                  bundle + sony_image + MB86H_TORNE_BOOT0_HOOK_ADDRESS - MB86H_BOOT0_BASE, 8));
    memset(m.torne_ts_ring, 0x5a, sizeof(m.torne_ts_ring));
    m.writes = 0;
    m.fail_after = 0;
    CHECK(mb86H_loader_restore(&loader, &ops, &m) == -5);
    CHECK(loader.torne_ts_ring_snapshot && loader.torne_ts_ring_saved);
    m.fail_after = -1;
    CHECK(!mb86H_loader_restore(&loader, &ops, &m));
    CHECK(m.block_writes[0] == 1);
    /* Session shutdown is issued once; SERVICE performs the bounded drain. */
    CHECK(m.shutdown_calls == 1u);
    CHECK(m.shutdown_services == (initialize ? 3u : 0u));
    for (i = 0; i < MB86H_RESIDENT_BYTES; ++i)
        CHECK(m.resident[i] == (unsigned char)(i * 11u));
    for (i = 0; i < MB86H_TORNE_TS_RING_BYTES; ++i)
        CHECK(m.torne_ts_ring[i] == (unsigned char)(i * 17u));
    CHECK(!memcmp(m.boot + MB86H_TORNE_BOOT0_CODE_ADDRESS,
                  bundle + sony_original + MB86H_TORNE_BOOT0_CODE_ADDRESS - MB86H_BOOT0_BASE,
                  MB86H_TORNE_BOOT0_CODE_BYTES));
    CHECK(!memcmp(m.boot + MB86H_BOOT0_STAGING_ADDRESS, staging, sizeof(staging)));
    for (i = 0; i < MB86H_TORNE_BOOT0_BODY_BYTES; ++i)
        CHECK(m.boot[MB86H_TORNE_BOOT0_BODY_ADDRESS + i] == 0x3c);
    if (initialize) {
        struct mb86H_loader fresh;
        unsigned images, windows, inits;
        CHECK(!mb86H_loader_start(&loader, &ops, &m));
        images = m.image_writes; windows = m.window_writes; inits = m.initialize_calls;
        /* A new host has no pointer to the old host's snapshots. */
        mb86H_session_reset(&session);
        CHECK(!mb86H_loader_parse_target(&fresh, bundle, total, true));
        CHECK(!mb86H_loader_start(&fresh, &ops, &m));
        CHECK(fresh.adopted && fresh.resident_active);
        CHECK(!fresh.resident_saved && !fresh.torne_ts_ring_saved);
        CHECK(m.recover_calls == 3 && m.initialize_calls == inits + 1);
        CHECK(m.image_writes == images && m.window_writes == windows);
        fresh.resident_active = true;
        CHECK(!mb86H_loader_restore(&fresh, &ops, &m));
        CHECK(m.image_writes == images && m.window_writes == windows);
        CHECK(!memcmp(m.boot + MB86H_TORNE_BOOT0_HOOK_ADDRESS,
            bundle + sony_image + MB86H_TORNE_BOOT0_HOOK_ADDRESS - MB86H_BOOT0_BASE, 8));
        /* Test process cleanup only; never replay a lost host's snapshots. */
        free(loader.resident_saved); free(loader.torne_boot0_saved);
        free(loader.torne_ts_ring_saved);
    }
    free(bundle);
    return 0;
}

int main(void)
{
    struct mock m = { .fail_after = -1, .capability_mask = 3 };
    struct mb86H_session session;
    struct mb86H_session_ops session_ops = { exchange, NULL, now_ms, delay_ms };
    struct mb86H_loader_ops ops = { .window_read = window_read,
        .window_write = window_write, .session = &session,
        .session_ops = &session_ops, .now_ms = now_ms, .delay_ms = delay_ms,
        .allocate = allocate, .release = release, .mismatch = mismatch };
    struct mb86H_loader loader;
    size_t boot_size = MB86H_BOOT0_BODY_OFFSET + sizeof(loader.body);
    size_t original = 64, image = original + boot_size;
    size_t resident = image + boot_size, total = resident + MB86H_RESIDENT_BYTES;
    unsigned char *bundle = calloc(1, total);
    unsigned char *before = malloc(MB86H_RESIDENT_BYTES);
    unsigned i;
    CHECK(bundle && before);
    memcpy(bundle, "XZFB", 4); mb86H_debug_u32be_put(bundle + 4, 1);
    mb86H_debug_u32be_put(bundle + 8, 64); mb86H_debug_u32be_put(bundle + 12, MB86H_BOOT0_BASE);
    mb86H_debug_u32be_put(bundle + 16, boot_size); mb86H_debug_u32be_put(bundle + 20, original);
    mb86H_debug_u32be_put(bundle + 24, image); mb86H_debug_u32be_put(bundle + 28, MB86H_RESIDENT_BASE);
    mb86H_debug_u32be_put(bundle + 32, MB86H_RESIDENT_BYTES); mb86H_debug_u32be_put(bundle + 36, resident);
    mb86H_debug_u32be_put(bundle + 48, total);
    for (i = 0; i < boot_size; ++i) bundle[original + i] = (unsigned char)(i * 3u);
    memset(bundle + image, 0xa5, boot_size); memset(bundle + resident, 0x5a, MB86H_RESIDENT_BYTES);
    m.expected_hook = bundle + image + MB86H_BOOT0_HOOK_OFFSET;
    make_resident_image(bundle + resident);
    memcpy(m.boot + MB86H_BOOT0_BASE, bundle + original, boot_size);
    memcpy(m.boot + MB86H_BOOT0_SIGNATURE_ADDRESS, "\xb4\x27\x9f\x8d\0\x0c\x46\0", 8);
    for (i = 0; i < MB86H_RESIDENT_BYTES; ++i) m.resident[i] = (unsigned char)(i * 7u);
    memcpy(before, m.resident, MB86H_RESIDENT_BYTES);
    mb86H_session_reset(&session);
    CHECK(!mb86H_loader_parse(&loader, bundle, total));
    /* Deployed XZFB has no DT295 profile; admission fails before I/O. */
    CHECK(mb86H_device_firmware_parse(mb86H_device_find(0x06b8, 0x103b),
                                      &loader, bundle, total) == -EINVAL);
    CHECK(!mb86H_device_firmware_parse(mb86H_gv_mvp_xz_device(), &loader, bundle, total));
    CHECK(!mb86H_loader_start(&loader, &ops, &m));
    CHECK(!m.oversized_window_reads);
    CHECK(m.ep0_writes[0] == 2 && m.ep0_writes[1] == 2);
    CHECK(!memcmp(m.resident, bundle + resident, MB86H_RESIDENT_BYTES));
    {
        unsigned images=m.image_writes,windows=m.window_writes;
        m.cold_required=m.reinitializable=true;
        CHECK(mb86H_loader_restore(&loader,&ops,&m)==-EBUSY);
        CHECK(loader.resident_active && loader.resident_saved);
        CHECK(m.image_writes==images && m.window_writes==windows);
        m.cold_required=m.reinitializable=false; /* Separate mock state; not a device recovery recipe. */
    }
    CHECK(!mb86H_loader_restore(&loader, &ops, &m));
    CHECK(!memcmp(m.resident, before, MB86H_RESIDENT_BYTES));
    CHECK(m.ep0_writes[0] >= 4 && m.ep0_writes[1] >= 4);
    CHECK(!memcmp(m.boot + MB86H_BOOT0_HOOK_ADDRESS,
                  bundle + original + MB86H_BOOT0_HOOK_OFFSET, sizeof(loader.hook)));
    CHECK(!memcmp(m.boot + MB86H_BOOT0_BODY_ADDRESS,
                  bundle + original + MB86H_BOOT0_BODY_OFFSET, sizeof(loader.body)));
    {
        struct mb86H_loader dt={.target=MB86H_BOOTSTRAP_DT295,.resident_active=true,
            .resident_attempted=true,.hook_attempted=true,.body_attempted=true,
            .torne_ts_ring_snapshot=true};
        uint8_t *resident_copy=malloc(4),*boot_copy=malloc(4),*ring_copy=malloc(4);
        unsigned releases=m.releases,images=m.image_writes,windows=m.window_writes;
        CHECK(resident_copy && boot_copy && ring_copy);
        dt.resident_saved=resident_copy;dt.torne_boot0_saved=boot_copy;
        dt.torne_ts_ring_saved=ring_copy;
        m.cold_required=true;m.reinitializable=false;m.initialized=false;
        CHECK(mb86H_loader_close(&dt,&ops,&m)==-EBUSY);
        CHECK(dt.resident_active && !dt.adopted && dt.resident_saved==resident_copy &&
              dt.torne_boot0_saved==boot_copy && dt.torne_ts_ring_saved==ring_copy &&
              m.releases==releases && m.image_writes==images && m.window_writes==windows);
        m.reinitializable=true;
        CHECK(!mb86H_loader_close(&dt,&ops,&m));
        CHECK(dt.resident_active && dt.adopted && !dt.recovery_error &&
              !dt.resident_saved && !dt.torne_boot0_saved && !dt.torne_ts_ring_saved &&
              !dt.resident_attempted && !dt.hook_attempted && !dt.body_attempted &&
              m.releases==releases+3 && m.image_writes==images && m.window_writes==windows);
        m.cold_required=m.reinitializable=false;
    }
    CHECK(!test_torne(false));
    CHECK(!test_torne(true));
    CHECK(!memcmp(m.boot + 0xe9b54, bundle + original + 0x9b54, 8));
    CHECK(!memcmp(m.boot + 0xe9c08, bundle + original + 0x9c08, 8));
    CHECK(!loader.resident_saved && !loader.body_attempted && !loader.hook_attempted);
    {
        struct mb86H_loader replacement, original_view;
        unsigned char *update_bundle=malloc(total);
        unsigned snapshots;
        uint8_t *preimage;
        unsigned build=MB86H_RESIDENT_BUILD_ID-MB86H_RESIDENT_BASE;
        CHECK(update_bundle);
        memcpy(update_bundle,bundle,total);
        update_bundle[resident]^=0x5a;
        update_bundle[resident+build]^=0xa5;
        make_resident_image(update_bundle+resident);
        CHECK(!mb86H_loader_start(&loader,&ops,&m));
        snapshots=m.snapshots;preimage=loader.resident_saved;
        CHECK(!mb86H_loader_parse(&replacement,update_bundle,total));
        CHECK(!mb86H_loader_parse(&original_view,bundle,total));
        CHECK(!mb86H_loader_update_validate(&loader,&replacement));
        CHECK(!mb86H_loader_update(&loader,&ops,&m,&replacement));
        CHECK(loader.resident==replacement.resident && loader.resident_saved==preimage &&
              loader.resident_active && m.snapshots==snapshots);
        CHECK(!mb86H_loader_update(&loader,&ops,&m,&original_view));
        CHECK(loader.resident==original_view.resident && loader.resident_saved==preimage &&
              loader.resident_active && m.snapshots==snapshots);
        update_bundle[image+MB86H_BOOT0_HOOK_OFFSET]^=1;
        CHECK(!mb86H_loader_parse(&replacement,update_bundle,total));
        unsigned image_writes=m.image_writes, window_writes=m.window_writes;
        CHECK(mb86H_loader_update_validate(&loader,&replacement)==-129);
        CHECK(m.image_writes==image_writes && m.window_writes==window_writes &&
              m.snapshots==snapshots);
        CHECK(mb86H_loader_update(&loader,&ops,&m,&replacement)==-129);
        CHECK(loader.resident==original_view.resident && loader.resident_active &&
              loader.resident_saved==preimage && m.image_writes==image_writes &&
              m.window_writes==window_writes && m.snapshots==snapshots);
        /* Once CAPS enters new code, even a failed admission must retain an
         * active owner so final restore cannot overwrite it without shutdown. */
        update_bundle[image+MB86H_BOOT0_HOOK_OFFSET]^=1;
        CHECK(!mb86H_loader_parse(&replacement,update_bundle,total));
        m.capability_mask=1;
        CHECK(mb86H_loader_update(&loader,&ops,&m,&replacement)==-EPROTO);
        CHECK(loader.resident_active && loader.resident_saved==preimage &&
              loader.resident==original_view.resident && m.snapshots==snapshots);
        CHECK(!mb86H_loader_restore(&loader,&ops,&m));
        CHECK(!memcmp(m.resident,before,MB86H_RESIDENT_BYTES));
        m.capability_mask=3;
        free(update_bundle);
    }
    m.writes = 0; m.fail_after = 3;
    CHECK(mb86H_loader_start(&loader, &ops, &m) == -5);
    CHECK(loader.body_attempted && !loader.hook_attempted);
    m.fail_after = -1;
    CHECK(!mb86H_loader_restore(&loader, &ops, &m));
    CHECK(!loader.body_attempted);
    CHECK(!memcmp(m.boot + MB86H_BOOT0_BODY_ADDRESS,
                  bundle + original + MB86H_BOOT0_BODY_OFFSET, sizeof(loader.body)));
    {
        struct mb86H_loader fresh;
        unsigned images, windows, inits;
        CHECK(!mb86H_loader_start(&loader, &ops, &m));
        images = m.image_writes; windows = m.window_writes; inits = m.initialize_calls;
        mb86H_session_reset(&session);
        CHECK(!mb86H_loader_parse(&fresh, bundle, total));
        unsigned reads = m.resident_block_reads, recover = m.recover_calls;
        unsigned commit = MB86H_RESIDENT_COMMIT - MB86H_RESIDENT_BASE;
        m.mismatch_count = 0;
        m.resident[commit] ^= 1;
        CHECK(mb86H_loader_start(&fresh, &ops, &m) == -129);
        CHECK(!fresh.adopted && m.recover_calls == recover);
        CHECK(m.resident_block_reads == reads + 1);
        CHECK(m.mismatch_count == 1 &&
              m.mismatch_address == MB86H_RESIDENT_COMMIT &&
              m.mismatch_expected == bundle[resident + commit] &&
              m.mismatch_observed == m.resident[commit]);
        CHECK(m.image_writes == images && m.window_writes == windows);
        m.resident[commit] ^= 1;

        CHECK(!mb86H_loader_parse(&fresh, bundle, total));
        reads = m.resident_block_reads;
        unsigned meta = MB86H_RESIDENT_META - MB86H_RESIDENT_BASE;
        m.mismatch_count = 0;
        m.resident[meta + 7] ^= 1;
        CHECK(mb86H_loader_start(&fresh, &ops, &m) == -129);
        CHECK(!fresh.adopted && m.recover_calls == recover);
        CHECK(m.resident_block_reads == reads + 2);
        CHECK(m.mismatch_count == 1 &&
              m.mismatch_address == MB86H_RESIDENT_META + 7 &&
              m.mismatch_expected == bundle[resident + meta + 7] &&
              m.mismatch_observed == m.resident[meta + 7]);
        m.resident[meta + 7] ^= 1;

        CHECK(!mb86H_loader_parse(&fresh, bundle, total));
        reads = m.resident_block_reads;
        unsigned build = MB86H_RESIDENT_BUILD_ID - MB86H_RESIDENT_BASE;
        m.mismatch_count = 0;
        m.resident[build + 3] ^= 1;
        CHECK(mb86H_loader_start(&fresh, &ops, &m) == -129);
        CHECK(!fresh.adopted && m.recover_calls == recover);
        CHECK(m.resident_block_reads == reads + 3);
        CHECK(m.mismatch_count == 1 &&
              m.mismatch_address == MB86H_RESIDENT_BUILD_ID + 3 &&
              m.mismatch_expected == bundle[resident + build + 3] &&
              m.mismatch_observed == m.resident[build + 3]);
        m.resident[build + 3] ^= 1;

        CHECK(!mb86H_loader_parse(&fresh, bundle, total));
        reads = m.resident_block_reads;
        m.mismatch_count = 0;
        m.resident[0] ^= 1;
        CHECK(mb86H_loader_start(&fresh, &ops, &m) < 0);
        CHECK(!fresh.adopted && m.recover_calls == recover);
        CHECK(m.resident_block_reads > reads + 3);
        CHECK(m.mismatch_count && m.mismatch_address == MB86H_RESIDENT_BASE &&
              m.mismatch_expected == bundle[resident] &&
              m.mismatch_observed == m.resident[0]);
        CHECK(m.image_writes == images && m.window_writes == windows);
        m.resident[0] ^= 1;
        m.bad_capability_build = true;
        CHECK(mb86H_loader_start(&fresh, &ops, &m) < 0);
        CHECK(!fresh.adopted && m.recover_calls == recover);
        m.bad_capability_build = false;
        CHECK(!mb86H_loader_start(&fresh, &ops, &m));
        CHECK(fresh.adopted && !fresh.resident_saved);
        CHECK(m.recover_calls == 3 && m.initialize_calls == inits + 1);
        fresh.resident_active = true;
        CHECK(!mb86H_loader_restore(&fresh, &ops, &m));
        CHECK(m.image_writes == images && m.window_writes == windows);
        CHECK(!memcmp(m.boot + MB86H_BOOT0_HOOK_ADDRESS,
              bundle + image + MB86H_BOOT0_HOOK_OFFSET, sizeof(loader.hook)));
        {
            unsigned recover_before=m.recover_calls;
            mb86H_session_reset(&session);
            CHECK(!mb86H_loader_parse(&fresh,bundle,total));
            m.cold_required=true;
            CHECK(mb86H_loader_start(&fresh,&ops,&m)==-EBUSY);
            CHECK(fresh.adopted && fresh.recovery_error==-EBUSY);
            CHECK(m.recover_calls==recover_before+1);
            CHECK(m.image_writes==images && m.window_writes==windows);
            CHECK(mb86H_loader_restore(&fresh,&ops,&m)==-EBUSY);
            mb86H_session_reset(&session);
            CHECK(!mb86H_loader_parse(&fresh,bundle,total));
            m.reinitializable=true;
            CHECK(!mb86H_loader_start(&fresh,&ops,&m));
            CHECK(fresh.adopted && fresh.resident_active && !fresh.recovery_error);
            CHECK(m.recover_calls==recover_before+2 &&
                  m.image_writes==images && m.window_writes==windows);
            m.initialized=false;
            m.cold_required=m.reinitializable=false;
        }
        mb86H_session_reset(&session);
        CHECK(!mb86H_loader_parse(&fresh, bundle, total));
        m.recover_error = -32;
        unsigned recover_before=m.recover_calls;
        CHECK(mb86H_loader_start(&fresh, &ops, &m) == -32);
        CHECK(fresh.adopted && fresh.recovery_error == -32);
        CHECK(mb86H_loader_restore(&fresh, &ops, &m) == -32);
        CHECK(m.recover_calls == recover_before+1); /* No implicit replay after transport failure. */
        free(loader.resident_saved);
    }
    free(before); free(bundle);
    puts("loader tests passed");
    return 0;
}
