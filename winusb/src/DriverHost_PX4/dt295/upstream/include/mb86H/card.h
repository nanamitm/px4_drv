/* SPDX-License-Identifier: MIT */
/* Semantic smart-card v2 client. The transport serializes each command. */
#ifndef MB86H_CARD_H
#define MB86H_CARD_H
#include "mb86H/session.h"
struct mb86H_card_ops {
    int (*command)(void *, unsigned, const uint8_t[40], uint8_t[64]);
    uint32_t (*now_ms)(void *);
    void (*delay_ms)(void *, unsigned);
};
enum mb86H_card_state {
    MB86H_CARD_ACTIVE = 0, MB86H_CARD_INACTIVE = 1, MB86H_CARD_ABSENT = 2
};
struct mb86H_card_parameters {
    uint8_t protocol, inverse, fi_di, guard, wi, ifsc, bwi, cwi, crc, specific;
};
int mb86H_card_parse_atr(const uint8_t *, size_t, struct mb86H_card_parameters *);
/* Each reader has its own state. The caller serializes whole card operations;
 * the command callback serializes only one packet with other FW clients. */
struct mb86H_card {
    const struct mb86H_card_ops *ops;
    void *opaque;
    uint32_t generation;
    int failed, firmware_result;
    bool active;
    uint8_t atr[MB86H_CARD_MAX_ATR];
    size_t atr_length;
    struct mb86H_card_parameters parameters;
};
void mb86H_card_init(struct mb86H_card *, const struct mb86H_card_ops *, void *);
int mb86H_card_status(struct mb86H_card *, uint8_t *);
int mb86H_card_power_on(struct mb86H_card *, uint8_t *, size_t *);
int mb86H_card_reset(struct mb86H_card *, uint8_t *, size_t *);
int mb86H_card_power_off(struct mb86H_card *);
int mb86H_card_transfer(struct mb86H_card *, const uint8_t *, size_t, uint8_t *, size_t *);
#endif
