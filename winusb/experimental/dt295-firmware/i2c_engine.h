/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DTP_I2C_ENGINE_H
#define DTP_I2C_ENGINE_H
#include <stdint.h>

/* このレイアウトは補助CPU側で観察したもの。実機での意味の検証は残る。 */
enum { DTP_I2C_STATE = 0xfe0, DTP_I2C_CONTROL = 0xfe4,
       DTP_I2C_RECOVERY = 0xfe8, DTP_I2C_DATA = 0xff0, DTP_I2C_MAX = 40 };
enum dtp_i2c_result { DTP_I2C_OK, DTP_I2C_PENDING, DTP_I2C_NOT_READY,
    DTP_I2C_INVALID, DTP_I2C_BUSY, DTP_I2C_TIMEOUT,
    DTP_I2C_CONTROLLER_ERROR, DTP_I2C_OWNERSHIP_LOST, DTP_I2C_REOPEN_REQUIRED };
enum dtp_i2c_phase { DTP_I2C_IDLE, DTP_I2C_ADDRESS_TX, DTP_I2C_TX,
    DTP_I2C_ADDRESS_RX, DTP_I2C_RX, DTP_I2C_STOP, DTP_I2C_DONE, DTP_I2C_FAILED };

struct dtp_i2c_io {
    void *opaque;
    uint32_t (*read32)(void *opaque, uint32_t offset);
    void (*write32)(void *opaque, uint32_t offset, uint32_t value);
    /* 成立条件にはクロック、リセット、他CPUとの排他の実機確認が必要。 */
    int (*owned)(void *opaque);
};

struct dtp_i2c_engine {
    struct dtp_i2c_io io;
    enum dtp_i2c_phase phase;
    enum dtp_i2c_result result;
    uint32_t started_us, timeout_us, state, control;
    uint8_t address, tx_size, rx_size, tx_submitted, tx_completed, rx_completed;
    uint8_t tx[DTP_I2C_MAX], rx[DTP_I2C_MAX];
    uint8_t in_flight, requires_reset, stop_requested;
};

void dtp_i2c_init(struct dtp_i2c_engine *, const struct dtp_i2c_io *);
enum dtp_i2c_result dtp_i2c_start(struct dtp_i2c_engine *, uint32_t address,
    const uint8_t *tx, uint32_t tx_size, uint32_t rx_size, uint32_t now_us, uint32_t timeout_us);
/* completion_readyはHALが確認・消費した進行通知。時刻の更新だけで転送を進めない。 */
enum dtp_i2c_result dtp_i2c_poll(struct dtp_i2c_engine *, uint32_t now_us, int completion_ready);
#endif
