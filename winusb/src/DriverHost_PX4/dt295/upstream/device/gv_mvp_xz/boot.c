/* SPDX-License-Identifier: MIT */
/* Read-only XZ boot0 observation. Firmware loading belongs to core/loader.c. */
#include "mb86H/usb/boot.h"
#include "mb86H/regs.h"
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

static unsigned long long fingerprint(const uint8_t *p, size_t size)
{
    unsigned long long hash = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < size; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}
static int fail(struct usb_boot_result *result, int rc)
{
    result->failed_stage = result->stage;
    result->stage = USB_BOOT_STAGE_FAILED;
    result->status = rc;
    return rc;
}
static int read16(const struct usb_boot_io *io, void *opaque, uint32_t address,
                   uint16_t *value, struct usb_boot_result *result)
{
    ++result->control_count;
    return usb_native_read16(&io->native, opaque, address, value);
}
static int read_bytes(const struct usb_boot_io *io, void *opaque, uint32_t address,
                       void *data, size_t size, struct usb_boot_result *result)
{
    ++result->control_count;
    return usb_native_read24(&io->native, opaque, address, data, size);
}
static int xz_observe(const struct usb_boot_io *io, void *opaque,
                       struct usb_boot_result *result)
{
    uint8_t data[16];
    int rc;
    if (!io || !io->native.control || !result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    result->stage = USB_BOOT_STAGE_READ_STATE;
    rc = read16(io, opaque, MB86H_REG_BOOT_STATE, &result->rawstate, result);
    if (rc) return fail(result, rc);
    if (result->rawstate == 0x10u || result->rawstate == 0x11u) {
        result->stage = USB_BOOT_STAGE_RUNNING;
        return 0;
    }
    if (result->rawstate) return fail(result, -EALREADY);
    result->stage = USB_BOOT_STAGE_VERIFY_BOOT0;
    rc = read_bytes(io, opaque, 0xe9ce8u, data, 8, result);
    if (rc) return fail(result, rc);
    if (fingerprint(data, 8) != 0xce1c939aaeb27baeull) return fail(result, -EKEYREJECTED);
    rc = read_bytes(io, opaque, 0xe91e4u, data, 16, result);
    if (rc) return fail(result, rc);
    if (fingerprint(data, 16) != 0x7df1d1a09a7c2775ull) return fail(result, -EKEYREJECTED);
    rc = read_bytes(io, opaque, 0xe9a2cu, data, 4, result);
    if (rc) return fail(result, rc);
    if (memcmp(data, "\x00\xf0\x00\x00", 4)) return fail(result, -EKEYREJECTED);
    rc = read_bytes(io, opaque, 0xc4810u, data, 4, result);
    if (rc) return fail(result, rc);
    if (memcmp(data, "\x82\x00\x00\x00", 4)) return fail(result, -EKEYREJECTED);
    result->code_signature_verified = true;
    result->stage = USB_BOOT_STAGE_BOOT0_READY;
    return 0;
}
static int xz_prepare(const struct usb_boot_io *io, void *opaque,
                       struct usb_boot_result *result)
{
    int rc = xz_observe(io, opaque, result);
    if (!rc && result->rawstate) return fail(result, -EALREADY);
    return rc;
}
static int xz_read(const struct usb_boot_io *io, void *opaque,
                    uint32_t address, void *data, size_t size)
{
    return usb_xz_window(&io->native, opaque, address, data, size, false);
}
const struct usb_boot_ops mb86H_gv_mvp_xz_boot_ops = {
    .observe = xz_observe, .prepare = xz_prepare, .read = xz_read,
    .candidate_address = 0xe0000u, .candidate_size = 0x10000u,
};
