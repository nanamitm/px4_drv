/* SPDX-License-Identifier: MIT */
#ifndef MB86H_FIRMWARE_PING_H
#define MB86H_FIRMWARE_PING_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

enum mb86H_firmware_ping_result {
    MB86H_FIRMWARE_PING_SUPPORTED = 0,
    MB86H_FIRMWARE_PING_MALFORMED = -2100,
    MB86H_FIRMWARE_PING_WRONG_DEVICE = -2101,
    MB86H_FIRMWARE_PING_UNSUPPORTED_VERSION = -2102,
};

/* Accepts only the exact, non-NUL-terminated ASCII payload mAgicVISION_1. */
int mb86H_firmware_ping_validate(const uint8_t *data, size_t size);

#endif
