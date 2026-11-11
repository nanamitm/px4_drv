/* SPDX-License-Identifier: MIT */
/* One bounded native-image download. No transport or device policy here. */
#ifndef MB86H_NATIVE_DOWNLOAD_H
#define MB86H_NATIVE_DOWNLOAD_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 uint8_t;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define MB86H_NATIVE_IMAGE_MAX (4u * 1024u * 1024u)
enum mb86H_native_download_stage {
    MB86H_NATIVE_VALIDATE, MB86H_NATIVE_PREPARE, MB86H_NATIVE_TRANSFER,
    MB86H_NATIVE_ACTIVATE, MB86H_NATIVE_DONE, MB86H_NATIVE_FAILED,
};
struct mb86H_native_download_result {
    enum mb86H_native_download_stage stage, failed_stage;
    size_t image_size, confirmed_bytes, last_requested, last_actual;
    unsigned completed_chunks;
    int status;
    bool recovery_required;
};
struct mb86H_native_download_options {
    /* Called between operations. No reentry into the transport. */
    int (*cancelled)(void *);
    void (*progress)(void *, const struct mb86H_native_download_result *);
    void *opaque;
};
struct mb86H_native_download_ops {
    int (*validate)(void *, const uint8_t *, size_t);
    int (*prepare)(void *);
    /* actual must be reported even for a failed/partial transfer. No replay. */
    int (*write)(void *, const uint8_t *, size_t, size_t *actual);
    int (*activate)(void *);
    size_t chunk_size;
};
int mb86H_native_image_validate(const uint8_t *, size_t, const char magic[8]);
int mb86H_native_download(const struct mb86H_native_download_ops *, void *,
    const uint8_t *, size_t, const struct mb86H_native_download_options *,
    struct mb86H_native_download_result *);
#endif
