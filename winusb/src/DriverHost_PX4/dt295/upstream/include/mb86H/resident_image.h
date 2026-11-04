/* SPDX-License-Identifier: MIT */
/* Transport-neutral resident image upload protocol. */
#ifndef MB86H_RESIDENT_IMAGE_H
#define MB86H_RESIDENT_IMAGE_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 uint8_t;
typedef u32 uint32_t;
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#include "mb86H/session.h"

struct mb86H_resident_image_ops {
    struct mb86H_session *session;
    const struct mb86H_session_ops *session_ops;
    void (*mismatch)(void *opaque, uint32_t address,
                     uint32_t expected, uint32_t observed);
};

int mb86H_resident_image_validate(const uint8_t *image);
int mb86H_resident_image_block(const struct mb86H_resident_image_ops *ops,
                                  void *opaque, uint32_t offset, bool write,
                                  const uint8_t *src, uint8_t *dst,
                                  uint32_t length);
int mb86H_resident_image_verify(const struct mb86H_resident_image_ops *ops,
                                   void *opaque, const uint8_t *image,
                                   bool skip_commit);
/* The caller owns lifecycle state and must mark the image callable on success. */
int mb86H_resident_image_program(const struct mb86H_resident_image_ops *ops,
                                    void *opaque, const uint8_t *image);

#endif
