/* SPDX-License-Identifier: MIT */
#include "mb86H/device.h"

const struct mb86H_device_info *mb86H_device_find(uint16_t vendor, uint16_t product)
{
    const struct mb86H_device_info *devices[] = {
        mb86H_gv_mvp_xz_device(), mb86H_torne_device(),
    };
    unsigned i;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i)
        if (devices[i]->vendor == vendor && devices[i]->product == product)
            return devices[i];
    return mb86H_pix_dt295_device(vendor, product);
}

const char *mb86H_device_name(uint16_t vendor, uint16_t product)
{
    const struct mb86H_device_info *device = mb86H_device_find(vendor, product);
    return device ? device->name : "mb86H device";
}
