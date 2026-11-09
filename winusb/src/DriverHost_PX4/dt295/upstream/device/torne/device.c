/* SPDX-License-Identifier: MIT */
#include "mb86H/device.h"

static const struct mb86H_device_info sony_torne = {
    .name = "Sony torne CECH-ZD1 J ISDB-T Tuner",
    .vendor = USB_TORNE_VENDOR_ID,
    .product = USB_TORNE_PRODUCT_ID,
    .frontend_count = 1,
    .delivery_systems = 1U << MB86H_DVB_GR,
    .runtime_protocol = MB86H_RUNTIME_RESIDENT_V1,
    .firmware_target = MB86H_FIRMWARE_TORNE,
};

const struct mb86H_device_info *mb86H_torne_device(void)
{
    return &sony_torne;
}
