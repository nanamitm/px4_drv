/* SPDX-License-Identifier: MIT */
/* Reattach to an exact known USB image after the previous host disappeared.
 * No saved pre-crash bytes are fabricated and no unknown entry is executed. */
#include "mb86H/loader.h"
#include "mb86H/device_query.h"
#include "mb86H/resident_image.h"
#include "mb86H/protocol.h"
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


static int exact_region(const struct mb86H_loader *loader,
                        const struct mb86H_loader_ops *ops, void *opaque,
                        uint32_t address, uint32_t size,
                        uint16_t step)
{
    uint8_t got[64], expected[64];
    uint32_t offset;
    for (offset = 0; offset < size; offset += step) {
        uint16_t length = size - offset > step ? step :
                          (uint16_t)(size - offset);
        int rc = mb86H_loader_boot0_read(loader, address + offset, expected, length);
        if (rc) return rc;
        rc = ops->window_read(opaque, address + offset, got, length);
        if (rc) return rc;
        if (memcmp(got, expected, length)) return -EKEYREJECTED;
    }
    return 0;
}

static int read_hook(const struct mb86H_loader *loader,
                     const struct mb86H_loader_ops *ops, void *opaque,
                     uint32_t address, uint8_t *hook, uint32_t size)
{
    uint32_t offset;
    /* Keep the retained native boot0 read contract. In particular the 88-byte
     * XZ patch is not one native EP0 read; the existing installer reads words. */
    uint16_t step = mb86H_loader_native_profile(loader) ? 8 : 4;
    for (offset = 0; offset < size; offset += step) {
        int rc = ops->window_read(opaque, address + offset, hook + offset, step);
        if (rc) return rc;
    }
    return 0;
}

static int validate_resident(struct mb86H_loader *loader,
                             const struct mb86H_loader_ops *ops, void *opaque)
{
    struct mb86H_resident_image_ops image_ops = {
        .session = ops->session, .session_ops = ops->session_ops,
        .mismatch = ops->mismatch,
    };
    uint8_t bytes[MB86H_RESIDENT_BLOCK_BYTES];
    uint32_t offset;
    bool mismatched = false;
    /* Mutable state is deliberately excluded. Compare every executable byte,
     * commit/checksum, manifest and build ID against the supplied image. */
    for (offset = 0; offset < MB86H_RESIDENT_BYTES; offset += sizeof(bytes)) {
        uint32_t count = MB86H_RESIDENT_BYTES - offset, i;
        int rc;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        if (offset >= MB86H_RESIDENT_IMMUTABLE_END - MB86H_RESIDENT_BASE &&
            offset + count <= MB86H_RESIDENT_META - MB86H_RESIDENT_BASE)
            continue;
        rc = mb86H_resident_image_block(&image_ops, opaque, offset, false,
                                        NULL, bytes, count);
        if (rc) return rc;
        for (i = 0; i < count; ++i) {
            uint32_t position = offset + i;
            if (position < MB86H_RESIDENT_IMMUTABLE_END - MB86H_RESIDENT_BASE ||
                (position >= MB86H_RESIDENT_META - MB86H_RESIDENT_BASE &&
                 position < MB86H_RESIDENT_META - MB86H_RESIDENT_BASE + 24) ||
                position >= MB86H_RESIDENT_BUILD_ID - MB86H_RESIDENT_BASE) {
                if (bytes[i] != loader->resident[position]) {
                    if (ops->mismatch)
                        ops->mismatch(opaque, MB86H_RESIDENT_BASE + position,
                                      loader->resident[position], bytes[i]);
                    mismatched = true;
                }
            }
        }
    }
    return mismatched ? -EKEYREJECTED : 0;
}

/* Check the small, unambiguous identity anchors before reading the immutable
 * executable range.  This keeps an incompatible or partially committed image
 * from consuming a full 196 KiB resident scan, while preserving the complete
 * byte-for-byte scan for a candidate that passes these anchors. */
static int validate_resident_anchor(struct mb86H_loader *loader,
                                    const struct mb86H_loader_ops *ops,
                                    void *opaque, uint32_t address,
                                    uint32_t size)
{
    struct mb86H_resident_image_ops image_ops = {
        .session = ops->session, .session_ops = ops->session_ops,
        .mismatch = ops->mismatch,
    };
    uint8_t bytes[24];
    uint32_t offset = address - MB86H_RESIDENT_BASE, i;
    bool mismatched = false;
    int rc;

    if (size > sizeof(bytes) || (offset & 3) || (size & 3)) return -EINVAL;
    rc = mb86H_resident_image_block(&image_ops, opaque, offset, false,
                                    NULL, bytes, size);
    if (rc) return rc;
    for (i = 0; i < size; ++i) {
        if (bytes[i] != loader->resident[offset + i]) {
            if (ops->mismatch)
                ops->mismatch(opaque, address + i,
                              loader->resident[offset + i], bytes[i]);
            mismatched = true;
        }
    }
    return mismatched ? -EKEYREJECTED : 0;
}

static int validate_resident_anchors(struct mb86H_loader *loader,
                                     const struct mb86H_loader_ops *ops,
                                     void *opaque)
{
    int rc;
    rc = validate_resident_anchor(loader, ops, opaque,
                                  MB86H_RESIDENT_COMMIT, 4);
    if (rc) return rc;
    rc = validate_resident_anchor(loader, ops, opaque,
                                  MB86H_RESIDENT_META, 24);
    if (rc) return rc;
    return validate_resident_anchor(loader, ops, opaque,
                                   MB86H_RESIDENT_BUILD_ID, 8);
}

static int recovered_owner_released(struct mb86H_session *session,
                                    const struct mb86H_session_ops *ops,
                                    void *opaque,unsigned target)
{
    struct mb86H_source_state state;
    int rc=mb86H_device_query_state(session,ops,opaque,0,&state);
    if(rc)return rc;
    if(state.owner_error<0)return state.owner_error;
    int released=!(state.flags&(MB86H_DEVICE_STATE_STARTED|
                          MB86H_DEVICE_STATE_INITIALIZED|
                          MB86H_DEVICE_STATE_SHUTTING_DOWN|
                          MB86H_DEVICE_STATE_RESTORE_PENDING|
                          MB86H_DEVICE_STATE_OWNED|
                          MB86H_DEVICE_STATE_CAPTURING|
                          MB86H_DEVICE_STATE_STREAM_READY)) &&
           !state.capture_mask;
    if(target!=MB86H_BOOTSTRAP_DT295 && released &&
       (state.flags&MB86H_DEVICE_STATE_COLD_REQUIRED) &&
       !(state.flags&MB86H_DEVICE_STATE_REINITIALIZABLE))return -EBUSY;
    return released?1:0;
}

int mb86H_loader_adopt(struct mb86H_loader *loader,
                       const struct mb86H_loader_ops *ops, void *opaque)
{
    const struct mb86H_native_boot_profile *profile;
    uint32_t hook_address, hook_size, body_address, body_size, started;
    uint8_t hook[88], expected[88], reply[64], data[40] = {0};
    const uint8_t *original;
    int rc;

    if (!loader || !loader->bundle || !ops || !ops->window_read ||
        !ops->session || !ops->session_ops || !ops->session_ops->exchange ||
        !ops->now_ms || !ops->delay_ms)
        return -EINVAL;
    profile = mb86H_loader_native_profile(loader);
    hook_address = profile ? profile->hook :
                                      MB86H_BOOT0_HOOK_ADDRESS;
    hook_size = profile ? sizeof(loader->torne_hook) : sizeof(loader->hook);
    body_address = profile ? profile->body :
                                      MB86H_BOOT0_BODY_ADDRESS;
    body_size = profile ? sizeof(loader->torne_body) : sizeof(loader->body);
    if (!profile) {
        /* XZ replaces the native BC-IN copy loop itself. Once published,
         * reading its hook/body through BC-IN no longer reads that memory:
         * it dispatches the staged packet. PING is side-effect free; factory
         * boot0 echoes the staged request, the bootstrap returns OK/version1.
         * Resident executable bytes are still checked before any entry call. */
        rc = mb86H_session_exchange(ops->session, ops->session_ops, opaque,
                                    MB86H_DEBUG_PING, 0, 0, 0, data, reply);
        if (rc) return rc;
        if (mb86H_be32_get(reply + 8) == MB86H_DEBUG_PING && !mb86H_be32_get(reply + 12) &&
            !mb86H_be32_get(reply + 16) && !mb86H_be32_get(reply + 20) &&
            !memcmp(reply + 24, data, sizeof(data))) return 0;
        if (mb86H_be32_get(reply + 8) != MB86H_DEBUG_OK || mb86H_be32_get(reply + 12) != 1)
            return -EPROTO;
        goto gateway_ready;
    }
    rc = read_hook(loader, ops, opaque, hook_address, hook, hook_size);
    if (rc) return rc;
    original = mb86H_loader_boot0_reference(loader, hook_address, hook_size);
    if (!original) return -EINVAL;
    if (!memcmp(hook, original, hook_size)) return 0;
    rc = mb86H_loader_boot0_read(loader, hook_address, expected, hook_size);
    if (rc) return rc;
    if (memcmp(hook, expected, hook_size)) return -EKEYREJECTED;
    rc = exact_region(loader, ops, opaque, body_address, body_size,
                       profile ? 64 : 4);
    if (rc) return rc;
    if (!profile) {
        static const uint32_t ep0[] = {0xe9b54u, 0xe9c08u};
        unsigned i;
        for (i = 0; i < 2; ++i) {
            rc = exact_region(loader, ops, opaque, ep0[i], 8, 4);
            if (rc) return rc;
        }
    } else {
        uint8_t out[4];
        if (profile->out_patch) {
        rc = ops->window_read(opaque, profile->out_patch, out, 4);
        if (rc) return rc;
        if (mb86H_be32_get(out) != profile->out_value) return -EKEYREJECTED;
        }
        if (ops->block_writes) {
            rc = ops->block_writes(opaque, true);
            if (rc) return rc;
            loader->torne_block_writes = true;
        }
    }
    rc = mb86H_session_exchange(ops->session, ops->session_ops, opaque,
                                MB86H_DEBUG_PING, 0, 0, 0, data, reply);
    if (rc) return rc;
    if (mb86H_be32_get(reply + 8) != MB86H_DEBUG_OK || mb86H_be32_get(reply + 12) != 1)
        return -EPROTO;
gateway_ready:
    rc = validate_resident_anchors(loader, ops, opaque);
    if (rc) return rc;
    rc = validate_resident(loader, ops, opaque);
    if (rc) return rc;
    rc = mb86H_session_command(ops->session, ops->session_ops, opaque,
                               MB86H_RESIDENT_COMMAND_CAPABILITIES, 0, data, reply);
    if (rc) return rc;
    if (mb86H_be32_get(reply + 8) != MB86H_DEBUG_OK ||
        mb86H_be32_get(reply + 16) != MB86H_RESIDENT_ABI_VERSION ||
        memcmp(reply + 24, MB86H_FIRMWARE_IDENTITY, MB86H_FIRMWARE_IDENTITY_BYTES) ||
        memcmp(reply + 40, loader->resident +
               MB86H_RESIDENT_BUILD_ID - MB86H_RESIDENT_BASE, 8) ||
        reply[39] != (profile ? profile->rf_mask : 3))
        return -EKEYREJECTED;

    /* Identity and executable bytes have been proven before admitting any
     * state-changing call. RECOVER retires old work, never replays it. */
    loader->adopted = true;
    loader->resident_active = true;
    started = ops->now_ms(opaque);
    for (;;) {
        rc = mb86H_session_command(ops->session, ops->session_ops, opaque,
                                   MB86H_RESIDENT_COMMAND_RECOVER, 0, data, reply);
        if (rc) break;
        if (mb86H_be32_get(reply + 8) != MB86H_DEBUG_OK) { rc = -EIO; break; }
        rc=recovered_owner_released(ops->session,ops->session_ops,opaque,loader->target);
        if(rc<0)break;
        if(rc>0) {
            /* The image remains callable and owns future card operations. */
            return 1;
        }
        if (ops->now_ms(opaque) - started > MB86H_INIT_TIMEOUT_MS) {
            rc = -ETIMEDOUT;
            break;
        }
        ops->delay_ms(opaque, 10);
    }
    loader->recovery_error = rc;
    return rc;
}
