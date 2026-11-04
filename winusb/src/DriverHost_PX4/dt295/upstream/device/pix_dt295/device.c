/* SPDX-License-Identifier: MIT */
#include "mb86H/device.h"
#include "boot.h"

static const struct mb86H_device_info pix_dt295_devices[] = {
    {.name = "PIX-DTxxx", .vendor = 0x04c5u, .product = 0x202fu,
     .frontend_count = 0, .delivery_systems = 0, .usb_boot = &mb86H_pix_dt295_boot_ops,
     .runtime_protocol = MB86H_RUNTIME_NONE},
    {.name = "PIX-DT420", .vendor = 0x06b8u, .product = 0x1038u,
     .frontend_count = 0, .delivery_systems = 0, .usb_boot = &mb86H_pix_dt295_boot_ops,
     .runtime_protocol = MB86H_RUNTIME_NONE},
    {.name = "PIX-DT410", .vendor = 0x06b8u, .product = 0x1039u,
     .frontend_count = 0, .delivery_systems = 0, .usb_boot = &mb86H_pix_dt295_boot_ops,
     .runtime_protocol = MB86H_RUNTIME_NONE},
    {.name = "PIX-DT295", .vendor = MB86H_PIX_DT295_VENDOR_ID,
     .product = MB86H_PIX_DT295_PRODUCT_ID,
     .frontend_count = 1,
     .delivery_systems = (1U << MB86H_DVB_GR) | (1U << MB86H_DVB_BSCS),
     .usb_boot = &mb86H_pix_dt295_resident_boot_ops,
     .runtime_protocol = MB86H_RUNTIME_RESIDENT_V1, .firmware_target = MB86H_FIRMWARE_DT295},
    {.name = "PIX-DT295 W", .vendor = 0x06b8u, .product = 0x103cu,
     .frontend_count = 0, .delivery_systems = 0, .usb_boot = &mb86H_pix_dt295_boot_ops,
     .runtime_protocol = MB86H_RUNTIME_NONE},
};

const struct mb86H_device_info *mb86H_pix_dt295_device(uint16_t vendor,
                                                        uint16_t product)
{
    unsigned i;
    for (i = 0; i < sizeof(pix_dt295_devices) / sizeof(pix_dt295_devices[0]); ++i)
        if (pix_dt295_devices[i].vendor == vendor && pix_dt295_devices[i].product == product)
            return &pix_dt295_devices[i];
    return 0;
}
