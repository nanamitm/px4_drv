/* SPDX-License-Identifier: MIT */
#include "boot.h"
#include "mb86H/regs.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define PIX_DT295_UNKNOWN_MODEL (~0u)
#define PIX_DT295_STATE_ADDRESS MB86H_REG_BOOT_STATE
#define PIX_DT295_READY_ADDRESS MB86H_REG_BOOT_IRQ
#define PIX_DT295_PENDING_ADDRESS 0x901b0u

struct pix_dt295_profile_entry {
    uint16_t vendor, product;
    struct mb86H_pix_dt295_profile profile;
};

/* The generic DTxxx USB ID covers more than one product string, so its
 * native model must be identified after enumeration rather than guessed. */
static const struct pix_dt295_profile_entry pix_dt295_profiles[] = {
    {0x04c5u, 0x202fu, {PIX_DT295_UNKNOWN_MODEL, {0u, 0u}, 100000u, false, false}},
    {0x06b8u, 0x1038u, {6u, {8u, 25u}, 100000u, false, true}},
    {0x06b8u, 0x1039u, {5u, {8u, 25u}, 1500000u, true, true}},
    {0x06b8u, 0x103bu, {7u, {0u, 0u}, 100000u, false, false}},
    {0x06b8u, 0x103cu, {8u, {9u, 25u}, 100000u, false, true}},
};

const struct mb86H_pix_dt295_profile *mb86H_pix_dt295_profile_find(
    uint16_t vendor, uint16_t product)
{
    size_t i;
    for (i = 0; i < sizeof(pix_dt295_profiles) / sizeof(pix_dt295_profiles[0]); ++i)
        if (pix_dt295_profiles[i].vendor == vendor && pix_dt295_profiles[i].product == product)
            return &pix_dt295_profiles[i].profile;
    return 0;
}

static int pix_read(const struct usb_boot_io *io, void *opaque,
                    uint32_t address, void *buffer, size_t size)
{
    if (!io || !io->native.control || !buffer || !size || size > 0x10000u ||
        address >= 0x1000000u || size > 0x1000000u - address)
        return -EINVAL;
    return usb_native_read24(&io->native, opaque, address, buffer, size);
}

static int pix_read16(const struct usb_boot_io *io, void *opaque,
                      uint32_t address, uint16_t *value, unsigned *count)
{
    if (count) ++*count;
    return io ? usb_native_read16(&io->native, opaque, address, value) : -EINVAL;
}

static int pix_masked_command(const struct usb_boot_io *io, void *opaque,
                              uint32_t address, unsigned shift,
                              uint16_t mask, uint16_t value, unsigned *count)
{
    if (count) ++*count;
    return io ? usb_native_mask16(&io->native, opaque, address, shift, mask, value) : -EINVAL;
}

/* Read-only bootstrap classifier. This factory-driver-derived request still
 * needs target-device acceptance; it establishes no boot0 execution claim. */
static int pix_observe(const struct usb_boot_io *io, void *opaque,
                       struct usb_boot_result *result)
{
    int rc;
    if (!io || !io->native.control || !result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    result->stage = USB_BOOT_STAGE_READ_STATE;
    rc = pix_read16(io, opaque, PIX_DT295_STATE_ADDRESS, &result->rawstate,
                    &result->control_count);
    if (!rc && (result->rawstate == 0u || result->rawstate == 0xffffu))
        return 0;
    if (!rc) rc = -EALREADY;
    result->failed_stage = result->stage;
    result->stage = USB_BOOT_STAGE_FAILED;
    result->status = rc;
    return rc;
}

static int pix_prepare(const struct usb_boot_io *io, void *opaque,
                       struct usb_boot_result *result)
{
    uint8_t ready_bytes[6];
    int rc;
    rc = pix_observe(io, opaque, result);
    if (rc) return rc;
    if (result->rawstate == 0u) {
        result->stage = USB_BOOT_STAGE_BOOT0_READY;
        return 0;
    }
    if (result->rawstate != 0xffffu) { rc = -EALREADY; goto failed; }
    result->stage = USB_BOOT_STAGE_WAIT_READY;
    if (!io->receive) { rc = -EINVAL; goto failed; }
    rc = io->receive(opaque, 0x83u, ready_bytes, sizeof(ready_bytes), 500u);
    if (rc && rc != -ETIMEDOUT) goto failed;
    result->stage = USB_BOOT_STAGE_READ_READY;
    rc = pix_read16(io, opaque, PIX_DT295_READY_ADDRESS, &result->ready_status,
                    &result->control_count);
    if (rc) goto failed;
    if (!(result->ready_status & 4u)) { rc = -EAGAIN; goto failed; }
    result->stage = USB_BOOT_STAGE_ACK_READY;
    rc = pix_masked_command(io, opaque, PIX_DT295_READY_ADDRESS, 2u, 1u, 1u,
                            &result->control_count);
    if (rc) goto failed;
    result->stage = USB_BOOT_STAGE_READ_PENDING;
    rc = pix_read16(io, opaque, PIX_DT295_READY_ADDRESS, &result->ready_status,
                    &result->control_count);
    if (rc) goto failed;
    rc = pix_read16(io, opaque, PIX_DT295_PENDING_ADDRESS, &result->pending_status,
                    &result->control_count);
    if (rc) goto failed;
    result->stage = USB_BOOT_STAGE_ACK_PENDING;
    if (result->ready_status || result->pending_status) {
        rc = pix_masked_command(io, opaque, PIX_DT295_READY_ADDRESS, 0u,
                                result->ready_status, result->ready_status,
                                &result->control_count);
        if (rc) goto failed;
        rc = pix_masked_command(io, opaque, PIX_DT295_PENDING_ADDRESS, 0u,
                                result->pending_status, result->pending_status,
                                &result->control_count);
        if (rc) goto failed;
    }
    result->stage = USB_BOOT_STAGE_BOOT0_READY;
    return 0;
failed:
    result->failed_stage = result->stage;
    result->stage = USB_BOOT_STAGE_FAILED;
    result->status = rc;
    return rc;
}

static int download_failed(struct usb_boot_result *result, int rc)
{
    result->failed_stage = result->stage;
    result->stage = USB_BOOT_STAGE_FAILED;
    result->status = rc;
    return rc;
}

/* Original SYS 17ABC, main side only. Preserve the duplicated bit 1 clear. */
static int pix_download_prepare(const struct usb_boot_io *io, void *opaque,
                                struct usb_boot_result *result)
{
    static const unsigned clear_bits[] = {13,12,11,10,9,8,7,6,5,4,3,2,1,1,0};
    uint8_t ready[6];
    unsigned i;
    int rc = pix_prepare(io, opaque, result);
    if (rc) return rc;
    result->stage = USB_BOOT_STAGE_CONFIGURE_DOWNLOAD;
    rc = pix_masked_command(io, opaque, 0x90194u, 12u, 3u, 1u, &result->control_count);
    if (rc) goto failed;
    ++result->control_count;
    rc = usb_native_write16(&io->native, opaque, 0x90198u, 0x40u);
    if (rc) goto failed;
    for (i = 0; i < sizeof(clear_bits) / sizeof(clear_bits[0]); ++i) {
        rc = pix_masked_command(io, opaque, 0x90078u, clear_bits[i], 1u, 0u,
                                &result->control_count);
        if (rc) goto failed;
    }
    for (i = 12; i > 0; --i) {
        rc = pix_masked_command(io, opaque, 0x901b4u, i - 1u, 1u, 0u,
                                &result->control_count);
        if (rc) goto failed;
    }
    result->stage = USB_BOOT_STAGE_WAIT_READY;
    rc = io->receive(opaque, 0x83u, ready, sizeof(ready), 100u);
    if (rc) goto failed;
    result->stage = USB_BOOT_STAGE_READ_READY;
    rc = pix_read16(io, opaque, PIX_DT295_READY_ADDRESS, &result->ready_status,
                    &result->control_count);
    if (rc) goto failed;
    if (!(result->ready_status & 4u)) { rc = -EAGAIN; goto failed; }
    result->stage = USB_BOOT_STAGE_ACK_READY;
    rc = pix_masked_command(io, opaque, PIX_DT295_READY_ADDRESS, 2u, 1u, 1u,
                            &result->control_count);
    if (rc) goto failed;
    result->stage = USB_BOOT_STAGE_BOOT0_READY;
    return 0;
failed:
    return download_failed(result, rc);
}

/* Original SYS 17B72. Enable completion before the execute trigger. */
static int pix_download_finish(const struct usb_boot_io *io, void *opaque,
                               struct usb_boot_result *result)
{
    uint8_t ready[6];
    unsigned i;
    int rc, state_rc;
    result->stage = USB_BOOT_STAGE_ENABLE_COMPLETION;
    for (i = 14; i > 0; --i) {
        rc = pix_masked_command(io, opaque, 0x90078u, i - 1u, 1u,
                                i - 1u == 4u ? 0u : 1u, &result->control_count);
        if (rc) goto failed;
    }
    for (i = 12; i > 0; --i) {
        rc = pix_masked_command(io, opaque, 0x901b4u, i - 1u, 1u, 1u,
                                &result->control_count);
        if (rc) goto failed;
    }
    result->stage = USB_BOOT_STAGE_EXECUTE;
    rc = pix_masked_command(io, opaque, 0x90070u, 2u, 1u, 1u, &result->control_count);
    if (rc) goto failed;
    result->stage = USB_BOOT_STAGE_WAIT_COMPLETE;
    rc = io->receive(opaque, 0x83u, ready, sizeof(ready), 2000u);
    if (rc) goto failed;
    result->stage = USB_BOOT_STAGE_READ_COMPLETE;
    rc = pix_read16(io, opaque, PIX_DT295_READY_ADDRESS, &result->ready_status,
                    &result->control_count);
    if (rc) goto failed;
    if (!(result->ready_status & 4u)) { rc = -EAGAIN; goto failed; }
    rc = pix_read16(io, opaque, PIX_DT295_STATE_ADDRESS, &result->rawstate,
                    &result->control_count);
    if (rc) goto failed;
    state_rc = result->rawstate == 0x10u || result->rawstate == 0x11u ? 0 : -EPROTO;
    result->stage = USB_BOOT_STAGE_ACK_COMPLETE;
    rc = pix_masked_command(io, opaque, PIX_DT295_READY_ADDRESS, 2u, 1u, 1u,
                            &result->control_count);
    if (rc) goto failed;
    if (state_rc) {
        result->stage = USB_BOOT_STAGE_READ_COMPLETE;
        rc = state_rc; goto failed;
    }
    /* SYS +158 is host worker join, not another device request. */
    result->stage = USB_BOOT_STAGE_RUNNING;
    return 0;
failed:
    return download_failed(result, rc);
}

static const struct usb_boot_download_ops pix_download = {
    .magic = {'M','B','8','A','C','0','3','1'}, .endpoint = 0x02u,
    .chunk_size = 0x10000u, .timeout_ms = 300u,
    .prepare = pix_download_prepare, .finish = pix_download_finish,
};

const struct usb_boot_ops mb86H_pix_dt295_boot_ops = {
    .observe = pix_observe,
    .prepare = pix_prepare,
    .read = pix_read,
    /* Candidate only; no DT295 boot0 bytes or device execution are verified. */
    .candidate_address = 0x000e0000u,
    .candidate_size = 0x00010000u,
    .download = &pix_download,
};

/* Exact live DT295 binding; the other Pixela IDs retain observation only. */
static int dt295_window_read(const struct usb_boot_io *io, void *opaque,
    uint32_t address, void *buffer, size_t size)
{
    return io ? usb_native_window(&io->native, opaque, &usb_dt295_window_profile,
        address, buffer, size, false, false) : -EINVAL;
}
const struct usb_boot_ops mb86H_pix_dt295_resident_boot_ops = {
    .observe = pix_observe, .prepare = pix_prepare, .read = dt295_window_read,
    .candidate_address = 0xe0000u, .candidate_size = 0x10000u,
    .window = &usb_dt295_window_profile, .download = &pix_download,
};
