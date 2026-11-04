/* SPDX-License-Identifier: MIT */
/* Common resident loader; buses provide the bounded memory window operations. */
#ifndef MB86H_LOADER_H
#define MB86H_LOADER_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#include "mb86H/session.h"
#include "mb86H/bootstrap.h"

struct mb86H_loader;
struct mb86H_loader_ops {
    int (*window_read)(void *opaque, uint32_t address, void *data,
                       uint16_t length);
    int (*window_write)(void *opaque, uint32_t address, const void *data,
                        uint16_t length);
    struct mb86H_session *session;
    const struct mb86H_session_ops *session_ops;
    uint32_t (*now_ms)(void *opaque);
    void (*delay_ms)(void *opaque, unsigned int milliseconds);
    void *(*allocate)(void *opaque, size_t size);
    void (*release)(void *opaque, void *memory);
    /* Optional target transport mode for bounded native 64-byte windows. */
    int (*block_writes)(void *opaque, bool enable);
    int (*snapshot)(void *opaque, const struct mb86H_loader *loader);
    void (*mismatch)(void *opaque, uint32_t address, uint32_t expected, uint32_t observed);
};

struct mb86H_native_boot_profile {
    uint32_t hook, body, code, code_bytes, staging, state;
    uint32_t out_patch, out_original, out_value, ring, ring_bytes;
    uint8_t rf_mask, state_bytes;
};

struct mb86H_loader {
    const uint8_t *bundle;
    const uint8_t *boot0_original;
    const uint8_t *boot0_image;
    const uint8_t *resident;
    struct mb86H_bootstrap bootstrap;
    uint32_t boot0_bytes;
    bool compact_bootstrap;
    uint8_t hook[88];
    uint8_t body[2236];
    uint8_t staging[64];
    uint8_t ep0_hook[2][8];
    uint8_t ep0_attempted;
    uint8_t *resident_saved;
    uint8_t *torne_boot0_saved;
    uint8_t *torne_ts_ring_saved;
    uint8_t torne_hook[8];
    uint8_t torne_body[0x800];
    uint8_t torne_out_original[4];
    uint8_t torne_state[4];
    uint8_t torne_execution[4];
    bool is_torne;
    unsigned target;
    bool torne_out_attempted;
    bool torne_block_writes;
    bool torne_ts_ring_snapshot;
    bool hook_attempted;
    bool body_attempted;
    bool resident_attempted;
    bool resident_active;
    /* A previous host vanished: original RAM snapshots do not exist here.
     * Release may quiesce this image, but must not invent/replay a preimage. */
    bool adopted;
    int recovery_error;
};

/* 0: pristine hook (normal install); 1: exact orphan recovered; negative:
 * uncertain/foreign state or failed recovery. Never retries failed I/O. */
int mb86H_loader_adopt(struct mb86H_loader *,
    const struct mb86H_loader_ops *, void *);

const struct mb86H_native_boot_profile *mb86H_loader_native_profile(const struct mb86H_loader *);

int mb86H_loader_parse(struct mb86H_loader *loader,
                           const uint8_t *bundle, size_t size);
/* Select a known target in the shared SBC2 or deployed XZFB container. */
int mb86H_loader_parse_target(struct mb86H_loader *loader,
                              const uint8_t *bundle, size_t size,
                              unsigned target);
/* The same bounded boot view serves installation and exact orphan recovery. */
const uint8_t *mb86H_loader_boot0_reference(const struct mb86H_loader *, uint32_t, size_t);
int mb86H_loader_boot0_read(const struct mb86H_loader *, uint32_t, void *, size_t);
/* Install and validate resident without initializing RF or a card. */
int mb86H_loader_install(struct mb86H_loader *,
    const struct mb86H_loader_ops *,void *);
int mb86H_loader_start(struct mb86H_loader *loader,
                           const struct mb86H_loader_ops *ops,
                           void *opaque);
/* Replace only a verified resident over the existing installed bootstrap.
 * The owner retains the original complete preimage for final restoration. */
int mb86H_loader_update(struct mb86H_loader *owner,
                        const struct mb86H_loader_ops *, void *opaque,
                        const struct mb86H_loader *replacement);
/* Pure OTA admission: validates target, resident image and every bootstrap
 * gateway byte. It performs no transport, allocation or snapshot operation. */
int mb86H_loader_update_validate(const struct mb86H_loader *owner,
                                 const struct mb86H_loader *replacement);
int mb86H_loader_restore(struct mb86H_loader *loader,
                             const struct mb86H_loader_ops *ops,
                             void *opaque);
/* Normal owner release. DT295 may retain an exact callable resident after a
 * successful reinitializable detach; explicit native restore remains the
 * separate loader_restore contract. */
int mb86H_loader_close(struct mb86H_loader *loader,
                       const struct mb86H_loader_ops *ops, void *opaque);

/* Canonical complete preimage payload, with no OS/file or header policy.
 * size returns zero for missing snapshots. write returns the first sink
 * error unchanged; each region is written once, with no partial retry. */
size_t mb86H_loader_snapshot_size(const struct mb86H_loader *);
int mb86H_loader_snapshot_write(const struct mb86H_loader *,
    int (*write)(void *, const void *, size_t), void *opaque);

#endif
