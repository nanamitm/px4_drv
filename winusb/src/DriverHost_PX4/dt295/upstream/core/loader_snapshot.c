/* SPDX-License-Identifier: MIT */
#include "mb86H/loader.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

struct snapshot_part { const void *data; size_t size; };

static int parts(const struct mb86H_loader *loader, struct snapshot_part out[9],
                  unsigned *count, size_t *size)
{
    unsigned n = 0, i;
    const struct mb86H_native_boot_profile *profile;
    if (!loader || !loader->resident_saved || loader->adopted) return -EINVAL;
    profile = mb86H_loader_native_profile(loader);
    if (profile && (!loader->torne_boot0_saved ||
        (profile->ring_bytes && (!loader->torne_ts_ring_snapshot || !loader->torne_ts_ring_saved)))) return -EINVAL;
#define ADD(p,nbytes) do { out[n++] = (struct snapshot_part){p, nbytes}; } while (0)
    if (profile) {
        ADD(loader->torne_hook, sizeof(loader->torne_hook));
        ADD(loader->torne_body, sizeof(loader->torne_body));
        if (profile->out_patch) ADD(loader->torne_out_original, sizeof(loader->torne_out_original));
        ADD(loader->torne_state, profile->state_bytes);
        ADD(loader->torne_execution, sizeof(loader->torne_execution));
        ADD(loader->torne_boot0_saved, profile->code_bytes);
    } else {
        ADD(loader->hook, sizeof(loader->hook));
        ADD(loader->body, sizeof(loader->body));
        ADD(loader->ep0_hook, sizeof(loader->ep0_hook));
    }
    ADD(loader->staging, sizeof(loader->staging));
    ADD(loader->resident_saved, MB86H_RESIDENT_BYTES);
    if (profile && profile->ring_bytes) ADD(loader->torne_ts_ring_saved, profile->ring_bytes);
#undef ADD
    *size = 0;
    for (i = 0; i < n; ++i) *size += out[i].size;
    *count = n;
    return 0;
}

size_t mb86H_loader_snapshot_size(const struct mb86H_loader *loader)
{
    struct snapshot_part list[9];
    unsigned count;
    size_t size;
    return parts(loader, list, &count, &size) ? 0 : size;
}

int mb86H_loader_snapshot_write(const struct mb86H_loader *loader,
    int (*write)(void *, const void *, size_t), void *opaque)
{
    struct snapshot_part list[9];
    unsigned count, i;
    size_t size;
    int rc;
    if (!write) return -EINVAL;
    rc = parts(loader, list, &count, &size);
    if (rc) return rc;
    for (i = 0; i < count; ++i) {
        rc = write(opaque, list[i].data, list[i].size);
        if (rc) return rc;
    }
    return 0;
}
