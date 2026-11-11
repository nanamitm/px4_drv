/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DTP_TORNE_HAL_H
#define DTP_TORNE_HAL_H
#include "i2c_engine.h"

/* アドレスはtorne CPU側の物理アドレス、値はホスト順。失敗時は0を返す。 */
struct dtp_torne_bus {
    void *opaque;
    int (*read32)(void *, uint32_t address, uint32_t *value);
    int (*write32)(void *, uint32_t address, uint32_t value);
    int (*write16)(void *, uint32_t address, uint16_t value);
    uint32_t (*now_us)(void *);
    /* generation一致、他CPU停止、クロック設定済み、排他保持を全て保証する。 */
    int (*exclusive)(void *, uint32_t generation);
};

struct dtp_torne_hal {
    struct dtp_torne_bus bus;
    struct dtp_i2c_engine engine;
    uint32_t generation, fault_address;
    enum dtp_i2c_result fault;
    uint8_t prepared;
};

/* 初期化はソフトウェア状態のみ。prepareは明示的に機器のI2Cをリセットする。 */
void dtp_torne_init(struct dtp_torne_hal *, const struct dtp_torne_bus *, uint32_t generation);
enum dtp_i2c_result dtp_torne_prepare(struct dtp_torne_hal *);
enum dtp_i2c_result dtp_torne_start(struct dtp_torne_hal *, uint32_t address7,
    const uint8_t *tx, uint32_t tx_size, uint32_t rx_size, uint32_t timeout_us);
enum dtp_i2c_result dtp_torne_poll(struct dtp_torne_hal *);
#endif
