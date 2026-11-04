/* SPDX-License-Identifier: MIT */
#include "mb86H/device.h"
#include "mb86H/loader.h"
#include "mb86H/resident_image.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

int mb86H_device_firmware_parse(const struct mb86H_device_info *device,
    struct mb86H_loader *loader, const uint8_t *bundle, size_t size)
{
    unsigned target;
    int rc;
    if (!device || !loader) return -EINVAL;
    if (device->runtime_protocol != MB86H_RUNTIME_RESIDENT_V1) return -EOPNOTSUPP;
    switch (device->firmware_target) {
    case MB86H_FIRMWARE_XZ: target = MB86H_BOOTSTRAP_XZ; break;
    case MB86H_FIRMWARE_TORNE: target = MB86H_BOOTSTRAP_TORNE; break;
    case MB86H_FIRMWARE_DT295: target = MB86H_BOOTSTRAP_DT295; break;
    default: return -EOPNOTSUPP;
    }
    rc = mb86H_loader_parse_target(loader, bundle, size, target);
    return rc ? rc : mb86H_resident_image_validate(loader->resident);
}
