/* SPDX-License-Identifier: MIT */
/* Device-specific match and loader contract. */
#ifndef MB86H_DEVICE_H
#define MB86H_DEVICE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif
#include "mb86H/protocol/constants.h"

/* The only Pixela DT target admitted by the resident Linux transport. */
#define MB86H_PIX_DT295_VENDOR_ID 0x06b8u
#define MB86H_PIX_DT295_PRODUCT_ID 0x103bu

struct usb_boot_ops;
struct mb86H_loader;
enum mb86H_runtime_protocol {
    MB86H_RUNTIME_NONE,
    MB86H_RUNTIME_RESIDENT_V1,
};
enum mb86H_firmware_target {
    MB86H_FIRMWARE_NONE,
    MB86H_FIRMWARE_XZ,
    MB86H_FIRMWARE_TORNE,
    MB86H_FIRMWARE_DT295,
};

struct mb86H_device_info {
    const char *name;
    uint16_t vendor;
    uint16_t product;
    uint8_t frontend_count;
    /* Bits indexed by enum mb86H_dvb_system; static RF hardware only. */
    uint8_t delivery_systems;
    enum mb86H_runtime_protocol runtime_protocol;
    enum mb86H_firmware_target firmware_target;
    const struct usb_boot_ops *usb_boot;
};

const struct mb86H_device_info *mb86H_gv_mvp_xz_device(void);
bool mb86H_gv_mvp_xz_matches(uint16_t vendor, uint16_t product);
const char *mb86H_gv_mvp_xz_device_name(uint16_t vendor, uint16_t product);
const struct mb86H_device_info *mb86H_torne_device(void);
const struct mb86H_device_info *mb86H_pix_dt295_device(uint16_t vendor, uint16_t product);
/* Exact identity lookup only; does not open or initialize a USB device. */
const struct mb86H_device_info *mb86H_device_find(uint16_t vendor, uint16_t product);
const char *mb86H_device_name(uint16_t vendor, uint16_t product);
/* One registry-selected target in the shared bundle; validates resident
 * integrity before any transport operation. No default target for NONE. */
int mb86H_device_firmware_parse(const struct mb86H_device_info *,
    struct mb86H_loader *, const uint8_t *bundle, size_t size);

#endif
