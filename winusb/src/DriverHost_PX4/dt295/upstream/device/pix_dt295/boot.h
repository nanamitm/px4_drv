/* SPDX-License-Identifier: MIT */
#ifndef MB86H_PIX_DT295_BOOT_H
#define MB86H_PIX_DT295_BOOT_H

#include "mb86H/usb/boot.h"

struct mb86H_pix_dt295_profile {
    unsigned native_model;
    uint8_t capacitance[2];
    uint32_t monitor_period;
    bool timeout_retry;
    bool capacitance_override;
};

const struct mb86H_pix_dt295_profile *mb86H_pix_dt295_profile_find(
    uint16_t vendor, uint16_t product);
extern const struct usb_boot_ops mb86H_pix_dt295_boot_ops;
extern const struct usb_boot_ops mb86H_pix_dt295_resident_boot_ops;

#endif
