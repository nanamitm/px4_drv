/* SPDX-License-Identifier: MIT */
#include "mb86H/loader.h"
#include "mb86H/resident_image.h"
#include "mb86H/protocol/constants.h"
#include "mb86H/bytes.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#ifndef EKEYREJECTED
#define EKEYREJECTED 129
#endif

static const struct mb86H_native_boot_profile native_profiles[] = {
    {MB86H_TORNE_BOOT0_HOOK_ADDRESS, MB86H_TORNE_BOOT0_BODY_ADDRESS,
     MB86H_TORNE_BOOT0_CODE_ADDRESS, MB86H_TORNE_BOOT0_CODE_BYTES,
     MB86H_BOOT0_STAGING_ADDRESS, MB86H_TORNE_BOOT0_STATE_ADDRESS,
     MB86H_TORNE_BOOT0_OUT_PATCH_ADDRESS, MB86H_TORNE_BOOT0_OUT_ORIGINAL,
     MB86H_TORNE_BOOT0_OUT_PATCH, MB86H_TORNE_TS_RING_BASE, MB86H_TORNE_TS_RING_BYTES, 1, 4},
    {MB86H_DT295_BOOT0_HOOK_ADDRESS, MB86H_DT295_BOOT0_BODY_ADDRESS,
     MB86H_DT295_BOOT0_CODE_ADDRESS, MB86H_DT295_BOOT0_CODE_BYTES,
     MB86H_DT295_BOOT0_STAGING_ADDRESS, MB86H_DT295_BOOT0_STATE_ADDRESS,
     0, 0, 0, 0, 0, 3, 2},
};
const struct mb86H_native_boot_profile *mb86H_loader_native_profile(const struct mb86H_loader *loader)
{
    if (loader->target == MB86H_BOOTSTRAP_DT295) return &native_profiles[1];
    return loader->is_torne ? &native_profiles[0] : 0;
}

static uint32_t resident_offset(uint32_t address)
{
    return address - MB86H_RESIDENT_BASE;
}

int mb86H_loader_parse_target(struct mb86H_loader *loader,
                              const uint8_t *bundle, size_t size,
                              unsigned target)
{
    bool is_torne = target == MB86H_BOOTSTRAP_TORNE;
    if (target > MB86H_BOOTSTRAP_DT295) return -EINVAL;
    uint32_t header, original, image, resident, total;
    const uint8_t *p = bundle;

    if (loader && bundle && size >= 4 && !memcmp(bundle, "SBC2", 4)) {
        struct mb86H_bootstrap view;
        const uint8_t *payload;
        int rc = mb86H_bootstrap_parse(&view, bundle, size, target, &payload);
        if (rc) return rc;
        memset(loader, 0, sizeof(*loader));
        loader->bundle = bundle;
        loader->resident = payload;
        loader->is_torne = is_torne;
        loader->target = target;
        loader->bootstrap = view;
        loader->compact_bootstrap = true;
        return 0;
    }

    if (target == MB86H_BOOTSTRAP_DT295) return -EINVAL;
    if (!loader || !bundle || size < 64 || memcmp(bundle, "XZFB", 4) ||
        mb86H_be32_get(bundle + 4) != 1)
        return -EINVAL;
    if (is_torne) {
        uint32_t descriptor;
        if (mb86H_be32_get(bundle + 52) != 0x544f524eu || mb86H_be32_get(bundle + 60) != 1)
            return -EINVAL;
        descriptor = mb86H_be32_get(bundle + 56);
        if (descriptor > size - 64) return -EINVAL;
        p = bundle + descriptor;
        if (memcmp(p, "TNFB", 4)) return -EINVAL;
    }
    if (mb86H_be32_get(p + 4) != 1)
        return -EINVAL;
    header = mb86H_be32_get(p + 8);
    original = mb86H_be32_get(p + 20);
    image = mb86H_be32_get(p + 24);
    resident = mb86H_be32_get(p + 36);
    total = mb86H_be32_get(p + 48);
    if (header != 64 || total != size || mb86H_be32_get(p + 12) != MB86H_BOOT0_BASE ||
        mb86H_be32_get(p + 28) != MB86H_RESIDENT_BASE ||
        mb86H_be32_get(p + 32) != MB86H_RESIDENT_BYTES ||
        mb86H_be32_get(p + 16) < (is_torne ?
            MB86H_TORNE_BOOT0_HOOK_ADDRESS - MB86H_BOOT0_BASE +
                MB86H_TORNE_BOOT0_HOOK_BYTES :
            MB86H_BOOT0_BODY_OFFSET + sizeof(loader->body)) ||
        (is_torne && mb86H_be32_get(p + 16) != 0x10000u) ||
        (is_torne && mb86H_be32_get(p + 16) <
            MB86H_TORNE_BOOT0_BODY_ADDRESS - MB86H_BOOT0_BASE +
                MB86H_TORNE_BOOT0_BODY_BYTES) ||
        original > size || image > size || resident > size ||
        mb86H_be32_get(p + 16) > size - original || mb86H_be32_get(p + 16) > size - image ||
        MB86H_RESIDENT_BYTES > size - resident ||
        (is_torne && resident != mb86H_be32_get(bundle + 36)))
        return -EINVAL;
    memset(loader, 0, sizeof(*loader));
    loader->bundle = bundle;
    loader->boot0_original = bundle + original;
    loader->boot0_image = bundle + image;
    loader->resident = bundle + resident;
    loader->boot0_bytes = mb86H_be32_get(p + 16);
    loader->is_torne = is_torne;
    loader->target = target;
    return 0;
}

int mb86H_loader_parse(struct mb86H_loader *loader,
                       const uint8_t *bundle, size_t size)
{
    return mb86H_loader_parse_target(loader, bundle, size, false);
}

const uint8_t *mb86H_loader_boot0_reference(const struct mb86H_loader *loader,
                                            uint32_t address, size_t size)
{
    if (loader->compact_bootstrap)
        return mb86H_bootstrap_reference(&loader->bootstrap, address, size);
    if (address < MB86H_BOOT0_BASE || address - MB86H_BOOT0_BASE > loader->boot0_bytes ||
        size > loader->boot0_bytes - (address - MB86H_BOOT0_BASE)) return 0;
    return loader->boot0_original + address - MB86H_BOOT0_BASE;
}

int mb86H_loader_boot0_read(const struct mb86H_loader *loader,
                            uint32_t address, void *out, size_t size)
{
    if (loader->compact_bootstrap)
        return mb86H_bootstrap_read(&loader->bootstrap, address, out, size);
    if (!out || address < MB86H_BOOT0_BASE || address - MB86H_BOOT0_BASE > loader->boot0_bytes ||
        size > loader->boot0_bytes - (address - MB86H_BOOT0_BASE)) return -EINVAL;
    memcpy(out, loader->boot0_image + address - MB86H_BOOT0_BASE, size);
    return 0;
}

static int window_read(const struct mb86H_loader_ops *ops, void *opaque,
                       uint32_t address, void *data, uint16_t length)
{
    return ops->window_read(opaque, address, data, length);
}

static int window_write(const struct mb86H_loader_ops *ops, void *opaque,
                        uint32_t address, const void *data, uint16_t length)
{
    return ops->window_write(opaque, address, data, length);
}

static int read_region(const struct mb86H_loader_ops *ops, void *opaque,
                       uint32_t address, uint8_t *out, const uint8_t *expected,
                       uint32_t length, uint16_t block_bytes)
{
    uint32_t offset;
    int rc;
    for (offset = 0; offset < length; offset += block_bytes) {
        uint32_t remaining = length - offset;
        uint16_t bytes = remaining > block_bytes ? block_bytes : (uint16_t)remaining;
        rc = window_read(ops, opaque, address + offset, out + offset, bytes);
        if (rc) return rc;
        if (expected && memcmp(out + offset, expected + offset, bytes)) {
            if (ops->mismatch)
                ops->mismatch(opaque, address + offset, mb86H_be32_get(expected + offset),
                              mb86H_be32_get(out + offset));
            return -EKEYREJECTED;
        }
    }
    return 0;
}

static int verify_region(const struct mb86H_loader_ops *ops, void *opaque,
                         uint32_t address, const uint8_t *expected,
                         uint32_t length, uint16_t block_bytes)
{
    uint8_t got[64];
    uint32_t offset;
    int rc;

    if (!expected || block_bytes > sizeof(got)) return -EINVAL;
    for (offset = 0; offset < length; offset += block_bytes) {
        uint32_t remaining = length - offset;
        uint16_t bytes = remaining > block_bytes ? block_bytes : (uint16_t)remaining;
        rc = window_read(ops, opaque, address + offset, got, bytes);
        if (rc) return rc;
        if (memcmp(got, expected + offset, bytes)) {
            if (ops->mismatch)
                ops->mismatch(opaque, address + offset, mb86H_be32_get(expected + offset),
                              mb86H_be32_get(got));
            return -EKEYREJECTED;
        }
    }
    return 0;
}

static int read_boot0_region(const struct mb86H_loader *loader,
    const struct mb86H_loader_ops *ops, void *opaque, uint32_t address,
    uint8_t *out, uint32_t size, uint16_t step)
{
    const uint8_t *expected = mb86H_loader_boot0_reference(loader, address, size);
    return expected ? read_region(ops, opaque, address, out, expected, size, step) : -EINVAL;
}

static int write_boot0_region(const struct mb86H_loader *loader,
    const struct mb86H_loader_ops *ops, void *opaque, uint32_t address, uint16_t size)
{
    uint8_t bytes[64];
    int rc;
    if (size > sizeof(bytes)) return -EINVAL;
    rc = mb86H_loader_boot0_read(loader, address, bytes, size);
    return rc ? rc : window_write(ops, opaque, address, bytes, size);
}

static struct mb86H_resident_image_ops resident_image_ops(
        const struct mb86H_loader_ops *ops)
{
    return (struct mb86H_resident_image_ops) {
        .session = ops->session,
        .session_ops = ops->session_ops,
        .mismatch = ops->mismatch,
    };
}

static int install_hook(struct mb86H_loader *loader,
                        const struct mb86H_loader_ops *ops, void *opaque)
{
    static const uint8_t signature[MB86H_BOOT0_SIGNATURE_BYTES] =
        {0xb4, 0x27, 0x9f, 0x8d, 0x00, 0x0c, 0x46, 0x00};
    static const uint32_t ep0[2] = {0xe9b54u, 0xe9c08u};
    uint8_t state[2], got[MB86H_BOOT0_SIGNATURE_BYTES];
    uint32_t offset;
    int rc;
    rc = window_read(ops, opaque, MB86H_BOOT0_STATE_ADDRESS, state, sizeof(state));
    if (rc || state[0] || state[1]) return rc ? rc : -EBUSY;
    rc = window_read(ops, opaque, MB86H_BOOT0_SIGNATURE_ADDRESS, got, sizeof(got));
    if (rc || memcmp(got, signature, sizeof(got))) return rc ? rc : -EKEYREJECTED;
    rc = read_boot0_region(loader, ops, opaque, MB86H_BOOT0_HOOK_ADDRESS,
                          loader->hook, sizeof(loader->hook), 4);
    if (rc) return rc;
    rc = read_boot0_region(loader, ops, opaque, MB86H_BOOT0_BODY_ADDRESS,
                          loader->body, sizeof(loader->body), 4);
    if (rc) return rc;
    for (offset = 0; offset < 2; ++offset) {
        rc = read_boot0_region(loader, ops, opaque, ep0[offset], loader->ep0_hook[offset], 8, 4);
        if (rc) return rc;
    }
    rc = window_read(ops, opaque, MB86H_BOOT0_STAGING_ADDRESS, loader->staging, 64);
    if (rc) return rc;
    for (offset = 0; offset < sizeof(loader->body); offset += 4) {
        loader->body_attempted = true;
        rc = write_boot0_region(loader, ops, opaque, MB86H_BOOT0_BODY_ADDRESS + offset, 4);
        if (rc) return rc;
    }
    for (offset = sizeof(loader->hook) - 4;; offset -= 4) {
        loader->hook_attempted = true;
        rc = write_boot0_region(loader, ops, opaque, MB86H_BOOT0_HOOK_ADDRESS + offset, 4);
        if (rc) return rc;
        if (!offset) break;
    }
    for (offset = 0; offset < 2; ++offset) {
        int part;
        for (part = 4; part >= 0; part -= 4) {
            loader->ep0_attempted |= 1u << offset;
            rc = write_boot0_region(loader, ops, opaque, ep0[offset] + part, 4);
            if (rc) return rc;
        }
    }
    return 0;
}

static int native_install_hook(struct mb86H_loader *loader,
                              const struct mb86H_loader_ops *ops, void *opaque)
{
    const struct mb86H_native_boot_profile *profile = mb86H_loader_native_profile(loader);
    uint8_t out_original[4], out_patch[4];
    for (unsigned i=0; i<4; ++i) {
        out_original[i]=(uint8_t)(profile->out_original >> (24-8*i));
        out_patch[i]=(uint8_t)(profile->out_value >> (24-8*i));
    }
    uint8_t verify[8];
    uint32_t offset;
    int rc;

    rc = read_region(ops, opaque, profile->state,
                     loader->torne_state, 0, profile->state_bytes, 4);
    if (rc) return rc;
    if (loader->torne_state[0] || loader->torne_state[1] ||
        loader->torne_state[2] || loader->torne_state[3])
        return -EBUSY;
    loader->torne_boot0_saved = ops->allocate(opaque, profile->code_bytes);
    if (!loader->torne_boot0_saved) return -ENOMEM;
    rc = read_boot0_region(loader, ops, opaque, profile->code,
                          loader->torne_boot0_saved, profile->code_bytes, 64);
    if (rc) return rc;
    memcpy(loader->torne_hook,
           loader->torne_boot0_saved + profile->hook -
               profile->code,
           sizeof(loader->torne_hook));
    rc = read_region(ops, opaque, profile->body,
                     loader->torne_body, 0,
                     sizeof(loader->torne_body), 64);
    if (rc) return rc;
    rc = read_region(ops, opaque, profile->staging, loader->staging,
                     0, sizeof(loader->staging), 64);
    if (rc) return rc;
    if (profile->out_patch) {
    rc = read_region(ops, opaque, profile->out_patch,
                     loader->torne_out_original, out_original,
                     sizeof(loader->torne_out_original), 4);
    if (rc) return rc;
    }
    rc = read_region(ops, opaque, 0x000fd0090u, loader->torne_execution,
                     0, sizeof(loader->torne_execution), 4);
    if (rc) return rc;
    if (profile->ring_bytes) {
    loader->torne_ts_ring_saved = ops->allocate(opaque, profile->ring_bytes);
    if (!loader->torne_ts_ring_saved) return -ENOMEM;
    rc = read_region(ops, opaque, profile->ring,
                     loader->torne_ts_ring_saved, 0,
                     profile->ring_bytes, 16384);
    if (rc) return rc;
    loader->torne_ts_ring_snapshot = true;
    }
    for (offset = 0; offset < MB86H_RESIDENT_BYTES; offset += 16384) {
        uint32_t length = MB86H_RESIDENT_BYTES - offset;
        if (length > 16384) length = 16384;
        rc = window_read(ops, opaque, MB86H_RESIDENT_BASE + offset,
                         loader->resident_saved + offset, length);
        if (rc) return rc;
    }
    if (ops->snapshot) { rc = ops->snapshot(opaque, loader); if (rc) return rc; }

    if (profile->out_patch) {
    loader->torne_out_attempted = true;
    rc = window_write(ops, opaque, profile->out_patch,
                      out_patch, sizeof(out_patch));
    if (rc) return rc;
    rc = read_region(ops, opaque, profile->out_patch,
                     verify, out_patch,
                     sizeof(out_patch), 4);
    if (rc) return rc;
    }
    if (ops->block_writes) {
        rc = ops->block_writes(opaque, true);
        if (rc) return rc;
        loader->torne_block_writes = true;
    }
    for (offset = 0; offset < sizeof(loader->torne_body); offset += 64) {
        loader->body_attempted = true;
        rc = write_boot0_region(loader, ops, opaque, profile->body + offset, 64);
        if (rc) return rc;
    }
    if (loader->target == MB86H_BOOTSTRAP_DT295) {
        uint8_t expected[64];
        for (offset=0; offset<sizeof(loader->torne_body); offset+=sizeof(expected)) {
            rc=mb86H_loader_boot0_read(loader,profile->body+offset,expected,sizeof(expected));
            if (!rc) rc=verify_region(ops,opaque,profile->body+offset,expected,sizeof(expected),64);
            if (rc) return rc;
        }
    }
    loader->hook_attempted = true;
    if (loader->target == MB86H_BOOTSTRAP_DT295) {
        rc = write_boot0_region(loader, ops, opaque, profile->hook, 8);
    } else {
        rc = write_boot0_region(loader, ops, opaque, profile->hook + 4, 4);
        if (rc) return rc;
        rc = write_boot0_region(loader, ops, opaque, profile->hook, 4);
    }
    if (rc) return rc;
    {
        uint8_t expected[8];
        rc = mb86H_loader_boot0_read(loader, profile->hook,
                                     expected, sizeof(expected));
        return rc ? rc : read_region(ops, opaque, profile->hook,
                                      verify, expected, sizeof(expected), 4);
    }
}

static int dispatch(struct mb86H_loader *loader,
                    const struct mb86H_loader_ops *ops, void *opaque,
                    uint32_t command, uint32_t channel,
                    const uint8_t data[40], uint8_t response[64])
{
    (void)loader;
    if (!ops->session || !ops->session_ops)
        return -EINVAL;
    return mb86H_session_command(ops->session, ops->session_ops, opaque,
                                    command, channel, data, response);
}

static int shutdown_resident(struct mb86H_loader *loader,
                             const struct mb86H_loader_ops *ops, void *opaque)
{
    int rc;
    if (!loader->resident_active)
        return 0;
    rc=loader->target==MB86H_BOOTSTRAP_DT295?
        mb86H_session_detach(ops->session,ops->session_ops,opaque):
        mb86H_session_shutdown(ops->session,ops->session_ops,opaque);
    if(!rc)loader->resident_active=false;
    return rc;
}

static int native_restore(struct mb86H_loader *loader,
                         const struct mb86H_loader_ops *ops, void *opaque)
{
    const struct mb86H_native_boot_profile *profile = mb86H_loader_native_profile(loader);
    uint32_t offset;
    int rc;

    rc = shutdown_resident(loader, ops, opaque);
    if (rc) return rc;
    if (!loader->torne_out_attempted && !loader->hook_attempted &&
        !loader->body_attempted && !loader->resident_attempted) {
        if (loader->resident_saved) ops->release(opaque, loader->resident_saved);
        if (loader->torne_boot0_saved) ops->release(opaque, loader->torne_boot0_saved);
        if (loader->torne_ts_ring_saved) ops->release(opaque, loader->torne_ts_ring_saved);
        loader->resident_saved = 0;
        loader->torne_boot0_saved = 0;
        loader->torne_ts_ring_saved = 0;
        loader->torne_ts_ring_snapshot = false;
        return 0;
    }
    /* SHUTDOWN already waited for all ownership to retire.  SERVICE's ready
     * predicate requires initialized=1, which normal shutdown clears. */
    if (loader->torne_ts_ring_snapshot && loader->torne_ts_ring_saved) {
        for (offset = 0; offset < profile->ring_bytes; offset += 16384) {
            uint32_t length = profile->ring_bytes - offset;
            if (length > 16384) length = 16384;
            rc = window_write(ops, opaque, profile->ring + offset,
                              loader->torne_ts_ring_saved + offset, length);
            if (rc) return rc;
        }
        rc = verify_region(ops, opaque, profile->ring,
                           loader->torne_ts_ring_saved,
                           profile->ring_bytes, 64);
        if (rc) return rc;
    }
    if (loader->hook_attempted) {
        if (loader->target == MB86H_BOOTSTRAP_DT295)
            rc = window_write(ops, opaque, profile->hook, loader->torne_hook, 8);
        else {
            rc = window_write(ops, opaque, profile->hook, loader->torne_hook, 4);
            if (!rc) rc = window_write(ops, opaque, profile->hook + 4,
                                       loader->torne_hook + 4, 4);
        }
        if (!rc) rc = verify_region(ops, opaque, profile->hook,
                                    loader->torne_hook, sizeof(loader->torne_hook), 4);
        if (rc) return rc;
    }
    if (loader->resident_attempted && loader->resident_saved) {
        for (offset = 0; offset < MB86H_RESIDENT_BYTES; offset += 16384) {
            uint32_t length = MB86H_RESIDENT_BYTES - offset;
            if (length > 16384) length = 16384;
            rc = window_write(ops, opaque, MB86H_RESIDENT_BASE + offset,
                              loader->resident_saved + offset, length);
            if (rc) break;
        }
        if (!rc) rc = verify_region(ops, opaque, MB86H_RESIDENT_BASE,
                                    loader->resident_saved,
                                    MB86H_RESIDENT_BYTES, 64);
        if (rc) return rc;
    }
    if (loader->body_attempted) {
        for (offset = 0; offset < sizeof(loader->torne_body); offset += 64) {
            rc = window_write(ops, opaque, profile->body + offset,
                              loader->torne_body + offset, 64);
            if (rc) return rc;
        }
        rc = verify_region(ops, opaque, profile->body,
                           loader->torne_body, sizeof(loader->torne_body), 64);
        if (rc) return rc;
    }
    if (loader->torne_out_attempted) {
        rc = window_write(ops, opaque, profile->out_patch,
                          loader->torne_out_original, sizeof(loader->torne_out_original));
        if (rc) return rc;
        rc = verify_region(ops, opaque, profile->out_patch,
                           loader->torne_out_original,
                           sizeof(loader->torne_out_original), 4);
        if (rc) return rc;
    }
    if (loader->torne_boot0_saved) {
        rc = verify_region(ops, opaque, profile->code,
                           loader->torne_boot0_saved,
                           profile->code_bytes, 64);
        if (rc) return rc;
    }
    rc = verify_region(ops, opaque, 0x000fd0090u, loader->torne_execution,
                       sizeof(loader->torne_execution), 4);
    if (rc) return rc;
    if (loader->torne_block_writes) {
        rc = ops->block_writes(opaque, false);
        if (rc) return rc;
        loader->torne_block_writes = false;
    }
    if (loader->body_attempted) {
        rc = window_write(ops, opaque, profile->staging + 4,
                          loader->staging + 4, 60);
        if (!rc) rc = window_write(ops, opaque, profile->staging,
                                   loader->staging, 4);
        if (!rc) rc = verify_region(ops, opaque, profile->staging,
                                    loader->staging, sizeof(loader->staging), 64);
        if (rc) return rc;
    }
    if (loader->resident_saved) ops->release(opaque, loader->resident_saved);
    if (loader->torne_boot0_saved) ops->release(opaque, loader->torne_boot0_saved);
    if (loader->torne_ts_ring_saved) ops->release(opaque, loader->torne_ts_ring_saved);
    loader->resident_saved = 0;
    loader->torne_boot0_saved = 0;
    loader->torne_ts_ring_saved = 0;
    loader->torne_ts_ring_snapshot = false;
    loader->resident_attempted = false;
    loader->hook_attempted = false;
    loader->body_attempted = false;
    loader->torne_out_attempted = false;
    return 0;
}

int mb86H_loader_install(struct mb86H_loader *loader,
                           const struct mb86H_loader_ops *ops, void *opaque)
{
    uint8_t response[64], data[40] = {0};
    struct mb86H_resident_image_ops image_ops;
    uint32_t offset;
    int rc;
    if (!loader || !loader->resident || !ops || !ops->window_read || !ops->window_write ||
        !ops->allocate || !ops->release || !ops->session || !ops->session_ops ||
        !ops->session_ops->exchange || !ops->session_ops->now_ms ||
        !ops->session_ops->delay_ms || !ops->now_ms || !ops->delay_ms) return -EINVAL;
    if (!loader->adopted && !loader->resident_saved && !loader->hook_attempted &&
        !loader->body_attempted && !loader->torne_out_attempted) {
        rc = mb86H_loader_adopt(loader, ops, opaque);
        if (rc) return rc < 0 ? rc : 0;
    }
    if (loader->adopted) return loader->recovery_error;
    if (mb86H_loader_native_profile(loader) && loader->resident_active) {
        rc = shutdown_resident(loader, ops, opaque);
        if (rc) return rc;
    }
    if (!mb86H_loader_native_profile(loader)) {
        rc = install_hook(loader, ops, opaque);
        if (rc) return rc;
    }
    image_ops = resident_image_ops(ops);
    loader->resident_saved = ops->allocate(opaque, MB86H_RESIDENT_BYTES);
    if (!loader->resident_saved) return -ENOMEM;
    for (offset = 0; !mb86H_loader_native_profile(loader) && offset < MB86H_RESIDENT_BYTES;
         offset += MB86H_RESIDENT_BLOCK_BYTES) {
        uint32_t length = MB86H_RESIDENT_BYTES - offset;
        if (length > MB86H_RESIDENT_BLOCK_BYTES) length = MB86H_RESIDENT_BLOCK_BYTES;
        rc = mb86H_resident_image_block(&image_ops, opaque, offset, false, 0,
                                        loader->resident_saved + offset, length);
        if (rc) { ops->release(opaque, loader->resident_saved); loader->resident_saved = 0; return rc; }
    }
    if (mb86H_loader_native_profile(loader)) {
        rc = native_install_hook(loader, ops, opaque);
        if (rc) return rc;
        if (loader->target == MB86H_BOOTSTRAP_DT295) {
            rc = mb86H_session_exchange(ops->session, ops->session_ops, opaque,
                MB86H_DEBUG_PING, 0, 0, 0, data, response);
            if (rc || mb86H_be32_get(response+8) != MB86H_DEBUG_OK || mb86H_be32_get(response+12) != 1)
                return rc ? rc : -EPROTO;
        }
    } else {
        if (ops->snapshot) { rc = ops->snapshot(opaque, loader); if (rc) return rc; }
    }
    loader->resident_attempted = true;
    rc = mb86H_resident_image_program(&image_ops, opaque, loader->resident);
    if (rc) return rc;
    loader->resident_active = true;
    rc = dispatch(loader, ops, opaque, MB86H_RESIDENT_COMMAND_CAPABILITIES, 0, data, response);
    if (rc || mb86H_be32_get(response + MB86H_DEBUG_PACKET_OPERATION_OFFSET) != MB86H_DEBUG_OK ||
        mb86H_be32_get(response + 16) != MB86H_RESIDENT_ABI_VERSION ||
        memcmp(response + MB86H_DEBUG_PACKET_DATA_OFFSET,
               MB86H_FIRMWARE_IDENTITY, MB86H_FIRMWARE_IDENTITY_BYTES) ||
        response[MB86H_DEBUG_PACKET_DATA_OFFSET + 15] !=
            (mb86H_loader_native_profile(loader) ? mb86H_loader_native_profile(loader)->rf_mask : 3))
        return rc ? rc : -EPROTO;
    return 0;
}

int mb86H_loader_start(struct mb86H_loader *loader,
    const struct mb86H_loader_ops *ops,void *opaque)
{
    int rc=mb86H_loader_install(loader,ops,opaque);
    if(rc)return rc;
    rc=mb86H_session_prepare(ops->session,ops->session_ops,opaque,0);
    if(rc)return rc;
    return mb86H_session_initialize(ops->session,ops->session_ops,opaque);
}

static int same_boot_view(const struct mb86H_loader *left,
                          const struct mb86H_loader *right,
                          uint32_t address, uint32_t bytes)
{
    uint8_t a[64], b[64];
    uint32_t offset;
    int rc;
    for(offset=0;offset<bytes;offset+=sizeof(a)) {
        uint32_t length=bytes-offset;
        if(length>sizeof(a))length=sizeof(a);
        rc=mb86H_loader_boot0_read(left,address+offset,a,length);
        if(!rc)rc=mb86H_loader_boot0_read(right,address+offset,b,length);
        if(rc)return rc;
        if(memcmp(a,b,length))return -EKEYREJECTED;
    }
    return 0;
}

static int same_bootstrap(const struct mb86H_loader *owner,
                          const struct mb86H_loader *replacement)
{
    const struct mb86H_native_boot_profile *profile=mb86H_loader_native_profile(owner);
    int rc;
    if(!profile) {
        rc=same_boot_view(owner,replacement,MB86H_BOOT0_HOOK_ADDRESS,88);
        if(!rc)rc=same_boot_view(owner,replacement,MB86H_BOOT0_BODY_ADDRESS,2236);
        if(!rc)rc=same_boot_view(owner,replacement,0xe9b54u,8);
        if(!rc)rc=same_boot_view(owner,replacement,0xe9c08u,8);
        return rc;
    }
    rc=same_boot_view(owner,replacement,profile->hook,8);
    if(!rc)rc=same_boot_view(owner,replacement,profile->body,0x800);
    if(!rc && profile->out_patch)
        rc=same_boot_view(owner,replacement,profile->out_patch,4);
    return rc;
}

static int update_ready(const struct mb86H_loader *owner)
{
    const struct mb86H_native_boot_profile *profile;
    if(!owner || owner->adopted || owner->recovery_error || !owner->resident_active ||
       !owner->resident_saved || !owner->resident_attempted || !owner->hook_attempted ||
       !owner->body_attempted)
        return -EINVAL;
    profile=mb86H_loader_native_profile(owner);
    if(!profile)return owner->ep0_attempted==3u?0:-EINVAL;
    if(!owner->torne_boot0_saved ||
       (profile->ring_bytes && (!owner->torne_ts_ring_snapshot ||
                                !owner->torne_ts_ring_saved)))
        return -EINVAL;
    return 0;
}

static int update_capabilities(struct mb86H_loader *owner,
                               const struct mb86H_loader_ops *ops, void *opaque,
                               const uint8_t *resident)
{
    const struct mb86H_native_boot_profile *profile=mb86H_loader_native_profile(owner);
    uint8_t response[64], data[40]={0};
    const uint8_t *build=resident+MB86H_RESIDENT_BUILD_ID-MB86H_RESIDENT_BASE;
    int rc=dispatch(owner,ops,opaque,MB86H_RESIDENT_COMMAND_CAPABILITIES,0,data,response);
    if(rc || mb86H_be32_get(response+MB86H_DEBUG_PACKET_OPERATION_OFFSET)!=MB86H_DEBUG_OK ||
       mb86H_be32_get(response+16)!=MB86H_RESIDENT_ABI_VERSION ||
       memcmp(response+MB86H_DEBUG_PACKET_DATA_OFFSET,
              MB86H_FIRMWARE_IDENTITY,MB86H_FIRMWARE_IDENTITY_BYTES) ||
       memcmp(response+MB86H_DEBUG_PACKET_DATA_OFFSET+16,build,8) ||
       memcmp(response+MB86H_DEBUG_PACKET_DATA_OFFSET+24,build,8) ||
       response[MB86H_DEBUG_PACKET_DATA_OFFSET+15]!=(profile?profile->rf_mask:3))
        return rc?rc:-EPROTO;
    return 0;
}

int mb86H_loader_update_validate(const struct mb86H_loader *owner,
                                 const struct mb86H_loader *replacement)
{
    int rc;
    if(!owner || !replacement || owner->target!=replacement->target ||
       owner->is_torne!=replacement->is_torne || !owner->resident ||
       !replacement->resident)
        return -EINVAL;
    rc=mb86H_resident_image_validate(replacement->resident);
    if(rc)return rc;
    return same_bootstrap(owner,replacement);
}

int mb86H_loader_update(struct mb86H_loader *owner,
                        const struct mb86H_loader_ops *ops, void *opaque,
                        const struct mb86H_loader *replacement)
{
    struct mb86H_resident_image_ops image_ops;
    int rc;
    if(!ops || !ops->session || !ops->session_ops || !ops->session_ops->exchange)
        return -EINVAL;
    rc=update_ready(owner);
    if(rc)return rc;
    rc=mb86H_loader_update_validate(owner,replacement);
    if(rc)return rc;
    rc=shutdown_resident(owner,ops,opaque);
    if(rc)return rc;
    image_ops=resident_image_ops(ops);
    rc=mb86H_resident_image_program(&image_ops,opaque,replacement->resident);
    if(rc)return rc;
    /* CAPS enters the new code. Even if its reply fails, restoration must
     * retire that potentially active owner before replacing original RAM. */
    owner->resident_active=true;
    rc=update_capabilities(owner,ops,opaque,replacement->resident);
    if(rc)return rc;
    owner->resident=replacement->resident;
    owner->resident_active=true;
    return 0;
}

int mb86H_loader_restore(struct mb86H_loader *loader,
                             const struct mb86H_loader_ops *ops, void *opaque)
{
    static const uint32_t ep0[2] = {0xe9b54u, 0xe9c08u};
    uint8_t zero[4] = {0}, got[4];
    struct mb86H_resident_image_ops image_ops;
    uint32_t offset, commit_offset = resident_offset(MB86H_RESIDENT_COMMIT);
    int rc = 0;
    if (!loader || !ops || !ops->window_read || !ops->window_write || !ops->release) return -EINVAL;
    if (loader->adopted) {
        if (loader->recovery_error) return loader->recovery_error;
        /* Original pre-crash host snapshots are gone. Retire ownership but
         * leave the validated inactive image for the next owner's adoption. */
        return shutdown_resident(loader, ops, opaque);
    }
    if (mb86H_loader_native_profile(loader)) return native_restore(loader, ops, opaque);
    rc = shutdown_resident(loader, ops, opaque);
    if (rc)
        return rc;
    if (loader->resident_attempted && loader->resident_saved) {
        image_ops = resident_image_ops(ops);
        rc = mb86H_resident_image_block(&image_ops, opaque, commit_offset,
                                           true, zero, 0, 4);
        for (offset = 0; !rc && offset < MB86H_RESIDENT_BYTES; offset += MB86H_RESIDENT_BLOCK_BYTES) {
            uint32_t length = MB86H_RESIDENT_BYTES - offset;
            if (length > MB86H_RESIDENT_BLOCK_BYTES) length = MB86H_RESIDENT_BLOCK_BYTES;
            if (offset <= commit_offset && commit_offset < offset + length) {
                uint32_t before = commit_offset - offset, after = before + 4;
                if (before) rc = mb86H_resident_image_block(&image_ops, opaque, offset, true, loader->resident_saved + offset, 0, before);
                if (!rc && after < length) rc = mb86H_resident_image_block(&image_ops, opaque, offset + after, true, loader->resident_saved + offset + after, 0, length - after);
            } else rc = mb86H_resident_image_block(&image_ops, opaque, offset, true, loader->resident_saved + offset, 0, length);
        }
        if (!rc) rc = mb86H_resident_image_block(&image_ops, opaque, commit_offset, true, loader->resident_saved + commit_offset, 0, 4);
        if (!rc) rc = mb86H_resident_image_block(&image_ops, opaque, commit_offset, false, 0, got, 4);
        if (!rc && memcmp(got, loader->resident_saved + commit_offset, 4)) rc = -EIO;
        if (!rc) rc = mb86H_resident_image_verify(&image_ops, opaque,
                                                     loader->resident_saved, false);
    }
    if (!rc && loader->hook_attempted)
        for (offset = 0; offset < sizeof(loader->hook); offset += 4) {
            rc = window_write(ops, opaque, MB86H_BOOT0_HOOK_ADDRESS + offset, loader->hook + offset, 4);
            if (rc) break;
        }
    for (offset = 0; !rc && offset < 2; ++offset)
        if (loader->ep0_attempted & (1u << offset)) {
            uint8_t verified[8];
            uint32_t part;
            for (part = 0; !rc && part < 8; part += 4)
                rc = window_write(ops, opaque, ep0[offset] + part,
                                  loader->ep0_hook[offset] + part, 4);
            if (!rc)
                rc = read_region(ops, opaque, ep0[offset], verified,
                                 loader->ep0_hook[offset], sizeof(verified), 4);
        }
    if (!rc && loader->body_attempted)
        for (offset = 0; offset < sizeof(loader->body); offset += 4) {
            rc = window_write(ops, opaque, MB86H_BOOT0_BODY_ADDRESS + offset, loader->body + offset, 4);
            if (rc) break;
        }
    if (!rc && loader->body_attempted)
        rc = window_write(ops, opaque, MB86H_BOOT0_STAGING_ADDRESS + 4, loader->staging + 4, 60);
    if (!rc && loader->body_attempted) {
        uint8_t state[2];
        rc = window_read(ops, opaque, MB86H_BOOT0_STATE_ADDRESS, state, sizeof(state));
        if (!rc && (state[0] || state[1])) rc = -EIO;
    }
    if (!rc) {
        if (loader->resident_saved) ops->release(opaque, loader->resident_saved);
        loader->resident_saved = 0;
        loader->resident_attempted = false;
        loader->ep0_attempted = 0;
        loader->hook_attempted = false;
        loader->body_attempted = false;
    }
    return rc;
}

int mb86H_loader_close(struct mb86H_loader *loader,
                       const struct mb86H_loader_ops *ops, void *opaque)
{
    int rc;
    if(!loader || !ops || !ops->release)return -EINVAL;
    if(loader->target!=MB86H_BOOTSTRAP_DT295 || !loader->resident_active)
        return mb86H_loader_restore(loader,ops,opaque);
    if(loader->recovery_error)return loader->recovery_error;
    if(!ops->session || !ops->session_ops)return -EINVAL;
    rc=mb86H_session_detach(ops->session,ops->session_ops,opaque);
    if(rc)return rc;
    if(loader->resident_saved)ops->release(opaque,loader->resident_saved);
    if(loader->torne_boot0_saved)ops->release(opaque,loader->torne_boot0_saved);
    if(loader->torne_ts_ring_saved)ops->release(opaque,loader->torne_ts_ring_saved);
    loader->resident_saved=0;
    loader->torne_boot0_saved=0;
    loader->torne_ts_ring_saved=0;
    loader->torne_ts_ring_snapshot=false;
    loader->resident_attempted=false;
    loader->hook_attempted=false;
    loader->body_attempted=false;
    loader->torne_out_attempted=false;
    loader->adopted=true;
    loader->recovery_error=0;
    /* The exact resident remains installed and callable for the next host. */
    loader->resident_active=true;
    return 0;
}
