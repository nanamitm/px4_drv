/* SPDX-License-Identifier: MIT */
#include "mb86H/native_download.h"
#include "mb86H/bytes.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif


int mb86H_native_image_validate(const uint8_t *data, size_t size,
                                const char magic[8])
{
    unsigned int sum = 0, i;
    if (!data || !magic || size < 544u || size > MB86H_NATIVE_IMAGE_MAX)
        return -EINVAL;
    if (memcmp(data, magic, 8)) return -ENOEXEC;
    for (i = 0; i < 32; i += 4) sum += mb86H_be32_get(data + i);
    if (sum || (size_t)mb86H_be32_get(data + 24) != size - 544u) return -EBADMSG;
    return 0;
}

static int checkpoint(const struct mb86H_native_download_options *options,
                       const struct mb86H_native_download_result *result)
{
    if (!options) return 0;
    if (options->progress) options->progress(options->opaque, result);
    return options->cancelled && options->cancelled(options->opaque) ? -ECANCELED : 0;
}

int mb86H_native_download(const struct mb86H_native_download_ops *ops, void *opaque,
    const uint8_t *image, size_t size,
    const struct mb86H_native_download_options *options,
    struct mb86H_native_download_result *result)
{
    bool prepared = false;
    int rc;
    if (!result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    result->stage = MB86H_NATIVE_VALIDATE;
    result->image_size = size;
    if (!ops || !ops->validate || !ops->prepare || !ops->write ||
        !ops->activate || !ops->chunk_size || !image || !size) {
        rc = -EINVAL; goto failed;
    }
    rc = ops->validate(opaque, image, size);
    if (rc) goto failed;
    rc = checkpoint(options, result);
    if (rc) goto failed;
    result->stage = MB86H_NATIVE_PREPARE;
    rc = checkpoint(options, result);
    if (rc) goto failed;
    /* A failed prepare can already have changed the device. */
    prepared = true;
    rc = ops->prepare(opaque);
    if (rc) goto failed;
    result->stage = MB86H_NATIVE_TRANSFER;
    while (result->confirmed_bytes < size) {
        size_t chunk = size - result->confirmed_bytes;
        rc = checkpoint(options, result);
        if (rc) goto failed;
        if (chunk > ops->chunk_size) chunk = ops->chunk_size;
        result->last_requested = chunk;
        result->last_actual = 0;
        rc = ops->write(opaque, image + result->confirmed_bytes, chunk,
                         &result->last_actual);
        if (rc) goto failed;
        if (result->last_actual != chunk) { rc = -EIO; goto failed; }
        result->confirmed_bytes += chunk;
        ++result->completed_chunks;
    }
    result->stage = MB86H_NATIVE_ACTIVATE;
    rc = checkpoint(options, result);
    if (rc) goto failed;
    rc = ops->activate(opaque);
    if (rc) goto failed;
    result->stage = MB86H_NATIVE_DONE;
    if (options && options->progress) options->progress(options->opaque, result);
    return 0;
failed:
    result->failed_stage = result->stage;
    result->stage = MB86H_NATIVE_FAILED;
    result->status = rc;
    result->recovery_required = prepared;
    if (options && options->progress) options->progress(options->opaque, result);
    return rc;
}
