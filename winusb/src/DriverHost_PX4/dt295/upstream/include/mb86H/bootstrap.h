/* SPDX-License-Identifier: MIT */
#ifndef MB86H_BOOTSTRAP_H
#define MB86H_BOOTSTRAP_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/stddef.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

enum mb86H_bootstrap_target { MB86H_BOOTSTRAP_XZ, MB86H_BOOTSTRAP_TORNE, MB86H_BOOTSTRAP_DT295 };

struct mb86H_bootstrap {
    const uint8_t *bundle;
    size_t size;
    const uint8_t *profile;
    const uint8_t *body;
    const uint8_t *relocations;
    const uint8_t *references;
    const uint8_t *patches;
    uint32_t body_size;
    uint32_t body_address;
    uint32_t body_capacity;
    uint32_t relocation_count;
    uint32_t reference_count;
    uint32_t patch_count;
    bool original_tail;
};

int mb86H_bootstrap_parse(struct mb86H_bootstrap *, const uint8_t *, size_t,
                          unsigned target, const uint8_t **resident);
const uint8_t *mb86H_bootstrap_reference(const struct mb86H_bootstrap *,
                                         uint32_t address, size_t length);
int mb86H_bootstrap_read(const struct mb86H_bootstrap *, uint32_t address,
                         void *out, size_t length);

#endif
