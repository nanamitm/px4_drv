/* SPDX-License-Identifier: MIT */
#include "mb86H/device.h"
#include "mb86H/protocol/constants.h"
#include "mb86H/usb/boot.h"

extern const struct usb_boot_ops mb86H_gv_mvp_xz_boot_ops;

static const struct mb86H_device_info gv_mvp_xz = {
    .name = "I-O DATA GV-MVP/XZ ISDB-T/S Tuner",
    .vendor = USB_VENDOR_ID,
    .product = USB_PRODUCT_ID,
    .frontend_count = 2,
    .delivery_systems = (1U << MB86H_DVB_GR) | (1U << MB86H_DVB_BSCS),
    .runtime_protocol = MB86H_RUNTIME_RESIDENT_V1,
    .firmware_target = MB86H_FIRMWARE_XZ,
    .usb_boot = &mb86H_gv_mvp_xz_boot_ops,
};

const struct mb86H_device_info *mb86H_gv_mvp_xz_device(void)
{
    return &gv_mvp_xz;
}

bool mb86H_gv_mvp_xz_matches(uint16_t vendor, uint16_t product)
{
    return vendor == gv_mvp_xz.vendor && product == gv_mvp_xz.product;
}

const char *mb86H_gv_mvp_xz_device_name(uint16_t vendor, uint16_t product)
{
    return mb86H_gv_mvp_xz_matches(vendor, product) ? gv_mvp_xz.name :
        "mb86H device";
}
