/* SPDX-License-Identifier: MIT */
#include "mb86H/usb/boot.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

struct download_context {
    const struct usb_boot_download_ops *profile;
    const struct usb_boot_io *io;
    void *opaque;
    struct usb_boot_result *boot;
};
static int validate(void *opaque, const uint8_t *image, size_t size)
{
    struct download_context *ctx = opaque;
    return usb_boot_validate_image(ctx->profile, image, size);
}
int usb_boot_validate_image(const struct usb_boot_download_ops *profile,
                            const uint8_t *image, size_t size)
{
    int rc;
    if (!profile) return -EINVAL;
    rc = mb86H_native_image_validate(image, size, profile->magic);
    return !rc && profile->validate_image ? profile->validate_image(image, size) : rc;
}
static int prepare(void *opaque)
{
    struct download_context *ctx = opaque;
    return ctx->profile->prepare(ctx->io, ctx->opaque, ctx->boot);
}
static int write_chunk(void *opaque, const uint8_t *data, size_t size, size_t *actual)
{
    struct download_context *ctx = opaque;
    return ctx->io->transmit(ctx->opaque, ctx->profile->endpoint, data, size,
                              actual, ctx->profile->timeout_ms);
}
static int activate(void *opaque)
{
    struct download_context *ctx = opaque;
    return ctx->profile->finish(ctx->io, ctx->opaque, ctx->boot);
}
int usb_boot_download(const struct usb_boot_download_ops *profile,
    const struct usb_boot_io *io, void *opaque, const uint8_t *image, size_t size,
    const struct mb86H_native_download_options *options,
    struct usb_boot_download_result *result)
{
    struct download_context ctx;
    struct mb86H_native_download_ops ops;
    if (!result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    result->download.image_size = size;
    if (!profile || !io || !io->native.control || !io->receive || !io->transmit ||
        !profile->prepare || !profile->finish || !profile->timeout_ms ||
        !profile->chunk_size || profile->chunk_size > 0x10000u ||
        !profile->endpoint || (profile->endpoint & 0x80u)) {
        result->download.stage = MB86H_NATIVE_FAILED;
        result->download.failed_stage = MB86H_NATIVE_VALIDATE;
        result->download.status = -EINVAL;
        return -EINVAL;
    }
    ctx = (struct download_context){profile, io, opaque, &result->boot};
    ops = (struct mb86H_native_download_ops){
        .validate = validate, .prepare = prepare, .write = write_chunk,
        .activate = activate, .chunk_size = profile->chunk_size,
    };
    return mb86H_native_download(&ops, &ctx, image, size, options, &result->download);
}
