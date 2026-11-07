/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PX4_DT295_RUNTIME_H
#define PX4_DT295_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct dt295_runtime;
enum dt295_target { DT295_TARGET_PIX_DT295, DT295_TARGET_TORNE };
/* control returns zero only after the exact transfer completed. */
struct dt295_io {
    int (*control)(void *, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t, unsigned);
    uint32_t (*now_ms)(void *);
    void (*delay_ms)(void *, unsigned);
    void (*observe)(void *, const uint8_t *, const uint8_t *, int);
    void *opaque;
    void (*mismatch)(void *, uint32_t address, uint32_t expected, uint32_t observed);
    int (*snapshot)(void *, const void *, size_t);
};
int dt295_validate(const uint8_t *, size_t);
int dt295_validate_target(const uint8_t *, size_t, enum dt295_target);
/* Firmware storage must outlive runtime. All commands require caller serialization. */
int dt295_create(struct dt295_runtime **, const uint8_t *, size_t, const struct dt295_io *);
int dt295_create_target(struct dt295_runtime **, const uint8_t *, size_t,
    const struct dt295_io *, enum dt295_target);
int dt295_start(struct dt295_runtime *);
int dt295_close(struct dt295_runtime *);
/* Diagnostic cleanup requires this owner's original snapshots; adoption is refused. */
int dt295_close_restore(struct dt295_runtime *);
/* destroy frees host storage only; caller first closes or records failed cleanup. */
void dt295_destroy(struct dt295_runtime *);
int dt295_tune(struct dt295_runtime *, unsigned source, uint32_t frequency_khz, uint32_t tsid);
int dt295_capture(struct dt295_runtime *, int enable);
int dt295_service(struct dt295_runtime *);
/* Diagnostic only: fixed TS ring MMIO read, caller holds the command lock. */
int dt295_diagnostic_ring(struct dt295_runtime *, uint32_t words[4]);
int dt295_locked(struct dt295_runtime *, int *);
int dt295_feed(struct dt295_runtime *, const uint8_t *, size_t,
    int (*packet)(void *, const uint8_t *), void *);
uint32_t dt295_generation(const struct dt295_runtime *);
void dt295_input_stats(const struct dt295_runtime *, uint64_t *discarded, uint64_t *desyncs);
#ifdef __cplusplus
}
#endif
#endif
