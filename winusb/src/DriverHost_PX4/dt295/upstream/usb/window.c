/* SPDX-License-Identifier: MIT */
#include "mb86H/usb/window.h"

#include "mb86H/protocol/constants.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

int usb_window_control_from_address(uint32_t address, size_t length,
                                    enum usb_window_direction direction,
                                    struct usb_window_control *out)
{
    if (!out || !length || length > USB_WINDOW_MAX_TRANSFER ||
        direction > USB_WINDOW_IN || address < USB_WINDOW_ADDRESS_MIN ||
        address >= USB_WINDOW_ADDRESS_MAX ||
        length > (size_t)(USB_WINDOW_ADDRESS_MAX - address))
        return -1;
    *out = (struct usb_window_control){
        .request_type = direction == USB_WINDOW_IN ? 0xC0u : 0x40u,
        .request = USB_WINDOW_VENDOR_REQUEST,
        .value = (uint16_t)((address >> 8) & 0xFF00u),
        .index = (uint16_t)(address & 0xffffu),
        .length = (uint16_t)length,
    };
    return 0;
}

int usb_window_address_from_control(const struct usb_window_control *control,
                                    uint32_t *address)
{
    if (!control || !address || control->request != USB_WINDOW_VENDOR_REQUEST ||
        control->value != USB_WINDOW_VENDOR_VALUE || !control->length ||
        control->length > USB_WINDOW_MAX_TRANSFER)
        return -1;
    *address = ((uint32_t)control->value << 8) | control->index;
    return *address >= USB_WINDOW_ADDRESS_MIN &&
                   *address < USB_WINDOW_ADDRESS_MAX &&
                   control->length <= USB_WINDOW_ADDRESS_MAX - *address
               ? 0
               : -1;
}


int usb_native_read24(const struct usb_native_ops *ops, void *opaque,
                      uint32_t address, void *buffer, size_t size)
{
    uint8_t *data = buffer;
    size_t offset;
    if (!ops || !ops->control || !data || !size || address >= 0x1000000U ||
        size > 0x1000000U - address) return -EINVAL;
    for (offset = 0; offset < size;) {
        size_t chunk = size - offset;
        uint32_t current = address + (uint32_t)offset;
        struct usb_window_control control;
        int rc;
        if (chunk > 64U) chunk = 64U;
        control = (struct usb_window_control) {
            .request_type = 0xc0, .request = USB_WINDOW_VENDOR_REQUEST,
            .value = (uint16_t)((current >> 8) & 0xff00U),
            .index = (uint16_t)current, .length = (uint16_t)chunk,
        };
        rc = ops->control(opaque, &control, data + offset, 1000);
        if (rc) return rc;
        offset += chunk;
    }
    return 0;
}

static void native_put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

int usb_native_read16(const struct usb_native_ops *ops, void *opaque,
                      uint32_t address, uint16_t *value)
{
    uint8_t bytes[2];
    int rc;
    if (!value || (address & 1u)) return -EINVAL;
    rc = usb_native_read24(ops, opaque, address, bytes, sizeof(bytes));
    if (!rc) *value = ((uint16_t)bytes[0] << 8) | bytes[1];
    return rc;
}

int usb_native_write16(const struct usb_native_ops *ops, void *opaque,
                       uint32_t address, uint16_t value)
{
    uint8_t bytes[2];
    struct usb_window_control control;
    if (!ops || !ops->control || address >= 0x1000000u || (address & 1u))
        return -EINVAL;
    native_put16(bytes, value);
    control = (struct usb_window_control){
        .request_type = 0x40u, .request = USB_WINDOW_VENDOR_REQUEST,
        .value = (uint16_t)((address >> 8) & 0xff00u),
        .index = (uint16_t)address, .length = 2u,
    };
    return ops->control(opaque, &control, bytes, 1000u);
}

int usb_native_mask16(const struct usb_native_ops *ops, void *opaque,
                      uint32_t address, unsigned shift, uint16_t mask, uint16_t value)
{
    uint8_t bytes[8] = {0};
    uint32_t aligned = address & ~3u;
    struct usb_window_control control;
    if (!ops || !ops->control || address >= 0x1000000u || (address & 1u) ||
        shift >= 16u || ((uint32_t)mask << shift) > 0xffffu ||
        ((uint32_t)value << shift) > 0xffffu) return -EINVAL;
    native_put16(bytes + (address & 2u), (uint16_t)(value << shift));
    native_put16(bytes + 4u + (address & 2u), (uint16_t)(mask << shift));
    control = (struct usb_window_control){
        .request_type = 0x40u, .request = USB_WINDOW_VENDOR_REQUEST,
        .value = (uint16_t)(((aligned >> 8) & 0xff00u) | 1u),
        .index = (uint16_t)aligned, .length = sizeof(bytes),
    };
    return ops->control(opaque, &control, bytes, 1000u);
}

/* Sony native BC transport, shared by libusb and Linux. No RF/MMIO policy. */
static int torne_native(const struct usb_native_ops *ops, void *opaque,
                        uint32_t address, uint8_t *data, size_t size,
                        bool writing, bool block_writes)
{
    size_t offset;
    if (!writing) return usb_native_read24(ops, opaque, address, data, size);
    if (!ops || !ops->control || !data || address >= 0x1000000U || !size ||
        size > 0x1000000U - address ||
        (writing && ((address & 3U) || (size & 3U))))
        return -EINVAL;
    if (writing && !block_writes && address == MB86H_BOOT0_STAGING_ADDRESS && size > 4U) {
        for (offset = 4; offset < size; offset += 4) {
            int rc = torne_native(ops, opaque, address + (uint32_t)offset,
                                   data + offset, 4, true, false);
            if (rc) return rc;
        }
        return torne_native(ops, opaque, address, data, 4, true, false);
    }
    for (offset = 0; offset < size;) {
        size_t chunk = writing && !block_writes ? 4U : size - offset;
        struct usb_window_control control;
        int rc;
        if (chunk > 64U) chunk = 64U;
        control = (struct usb_window_control) {
            .request_type = writing ? 0x40 : 0xc0,
            .request = USB_WINDOW_VENDOR_REQUEST,
            .value = (uint16_t)(((address + offset) >> 8) & 0xff00U),
            .index = (uint16_t)(address + offset), .length = (uint16_t)chunk,
        };
        rc = ops->control(opaque, &control, data + offset, 1000);
        if (rc) return rc;
        offset += chunk;
    }
    return 0;
}

const struct usb_native_window_profile usb_torne_window_profile = {
    MB86H_TORNE_BOOT0_READ_WINDOW, MB86H_TORNE_BOOT0_WRITE_WINDOW,
    MB86H_BOOT0_STAGING_ADDRESS, MB86H_TORNE_TS_RING_BASE,
    MB86H_TORNE_TS_RING_BYTES, false, 0, 0,
};
const struct usb_native_window_profile usb_dt295_window_profile = {
    MB86H_DT295_BOOT0_READ_WINDOW, MB86H_DT295_BOOT0_WRITE_WINDOW,
    MB86H_DT295_BOOT0_STAGING_ADDRESS, 0, 0, true,
    MB86H_DT295_AUX_RAM_ADDRESS, MB86H_DT295_AUX_RAM_BYTES,
};

int usb_native_window(const struct usb_native_ops *ops, void *opaque,
                     const struct usb_native_window_profile *profile,
                     uint32_t address, void *buffer, size_t size,
                     bool writing, bool block_writes)
{
    static const uint8_t native_window[4] = {0x00, 0xf0, 0x00, 0x00};
    uint8_t *data = buffer;
    uint32_t window;
    if (!profile) return -EINVAL;
    window = writing ? profile->write_window : profile->read_window;
    size_t cursor = 0;
    if (address < 0x1000000U)
        return torne_native(ops, opaque, address, data, size, writing, block_writes);
    if (!ops || !ops->control || !data || !size ||
        !((address >= MB86H_RESIDENT_BASE &&
           address < MB86H_RESIDENT_END &&
           size <= MB86H_RESIDENT_END - address) ||
          (profile->ring_bytes && address >= profile->ring_base &&
           address < profile->ring_base + profile->ring_bytes &&
           size <= profile->ring_base + profile->ring_bytes - address) ||
          (profile->memory_bytes && address >= profile->memory_base &&
           address < profile->memory_base + profile->memory_bytes &&
           size <= profile->memory_base + profile->memory_bytes - address)) ||
        (writing && ((address & 3U) || (size & 3U)))) return -EINVAL;
    while (cursor < size) {
        uint32_t current = address + (uint32_t)cursor;
        uint32_t bank = current & ~0x1ffffU;
        uint32_t alias = 0x80000U + (current & 0x1ffffU);
        size_t chunk = size - cursor;
        uint8_t mapped[4] = {bank >> 24, bank >> 16, bank >> 8, bank};
        uint8_t expected[4], restored[4];
        int rc, restore_rc;
        if (chunk > 0x20000U - (current & 0x1ffffU))
            chunk = 0x20000U - (current & 0x1ffffU);
        if (profile->dispatch_out) {
            rc = torne_native(ops, opaque, window, restored, 4, false, false);
            if (rc) return rc;
            if (memcmp(restored, native_window, 4)) return -EPROTO;
        }
        memcpy(expected, mapped, sizeof(expected));
        rc = torne_native(ops, opaque, window, mapped, 4, true, false);
        if (!rc) rc = torne_native(ops, opaque, window, mapped, 4, false, false);
        if (!rc && memcmp(mapped, expected, 4)) rc = -EIO;
        if (!rc) rc = torne_native(ops, opaque, alias, data + cursor, chunk,
                                    writing, writing && block_writes);
        /* Restore even when the mapped operation failed; never replay it. */
        restore_rc = torne_native(ops, opaque, window, (uint8_t *)native_window, 4, true, false);
        if (!restore_rc) restore_rc = torne_native(ops, opaque, window, restored, 4, false, false);
        if (!restore_rc && memcmp(restored, native_window, 4)) restore_rc = -EIO;
        if (restore_rc) return restore_rc;
        if (rc) return rc;
        cursor += chunk;
    }
    return 0;
}

int usb_native_exchange(const struct usb_native_ops *ops, void *opaque,
    const struct usb_native_window_profile *profile,
    const uint8_t request[64], uint8_t response[64], bool block_writes)
{
    struct usb_window_control dispatch;
    int rc;
    if (!profile || !ops || !ops->control || !request || !response) return -EINVAL;
    dispatch = (struct usb_window_control) {
        .request_type = profile->dispatch_out ? 0x40 : 0xc0,
        .request = USB_WINDOW_VENDOR_REQUEST,
        .value = (uint16_t)(((profile->staging >> 8) & 0xff00u) | 0x7du),
        .index = (uint16_t)profile->staging, .length = 64,
    };
    if (profile->dispatch_out) {
        rc = ops->control(opaque, &dispatch, (void *)request, 20000);
        return rc ? rc : usb_native_read24(ops, opaque, profile->staging, response, 64);
    }
    rc = torne_native(ops, opaque, profile->staging, (uint8_t *)request, 64, true, block_writes);
    return rc ? rc : ops->control(opaque, &dispatch, response, 20000);
}
int usb_torne_window(const struct usb_native_ops *ops, void *opaque,
    uint32_t address, void *buffer, size_t size, bool writing, bool block_writes)
{
    return usb_native_window(ops, opaque, &usb_torne_window_profile,
                              address, buffer, size, writing, block_writes);
}
int usb_torne_exchange(const struct usb_native_ops *ops, void *opaque,
    const uint8_t request[64], uint8_t response[64], bool block_writes)
{
    return usb_native_exchange(ops, opaque, &usb_torne_window_profile,
                                request, response, block_writes);
}


int usb_xz_window(const struct usb_native_ops *ops, void *opaque,
                  uint32_t address, void *buffer, size_t size, bool writing)
{
    uint8_t *data = buffer;
    size_t offset;
    if (!ops || !ops->control || !data || !size || address > 0xfffffu ||
        size > 0x100000u - address ||
        (writing && ((address & 3u) || (size & 3u)))) return -EINVAL;
    for (offset = 0; offset < size;) {
        size_t count = size - offset, limit = writing ? 4u : 64u;
        uint32_t current = address + (uint32_t)offset;
        struct usb_window_control control;
        int rc;
        if (count > limit) count = limit;
        control = (struct usb_window_control) {
            .request_type = writing ? 0x40 : 0xc0,
            .request = USB_WINDOW_VENDOR_REQUEST,
            .value = (uint16_t)(((current >> 16) & 15u) << 8),
            .index = (uint16_t)current, .length = (uint16_t)count,
        };
        rc = ops->control(opaque, &control, data + offset, 1000);
        if (rc) return rc;
        offset += count;
    }
    return 0;
}
int usb_xz_exchange(const struct usb_native_ops *ops, void *opaque,
                    const uint8_t request[64], uint8_t response[64])
{
    unsigned i;
    if (!ops || !ops->control || !request || !response) return -EINVAL;
    for (i = 0; i < 16; ++i) {
        uint32_t offset = 60 - i * 4;
        uint32_t address = MB86H_BOOT0_STAGING_ADDRESS + offset;
        const struct usb_window_control control = {
            .request_type = 0x40, .request = USB_WINDOW_VENDOR_REQUEST,
            .value = (uint16_t)(((address >> 16) & 15u) << 8),
            .index = (uint16_t)address, .length = 4,
        };
        int rc = ops->control(opaque, &control, (uint8_t *)request + offset, 20000);
        if (rc) return rc;
    }
    {
        const struct usb_window_control control = {
            .request_type = 0xc0, .request = USB_WINDOW_VENDOR_REQUEST,
            .value = (uint16_t)(((MB86H_BOOT0_STAGING_ADDRESS >> 16) & 15u) << 8),
            .index = (uint16_t)MB86H_BOOT0_STAGING_ADDRESS, .length = 64,
        };
        return ops->control(opaque, &control, response, 20000);
    }
}
