/* SPDX-License-Identifier: GPL-2.0-only */
#include "i2c_engine.h"

static void control(struct dtp_i2c_engine *e, uint32_t clear, uint32_t set)
{
    e->control = (e->control & ~clear) | set;
    e->io.write32(e->io.opaque, DTP_I2C_CONTROL, e->control);
}

void dtp_i2c_init(struct dtp_i2c_engine *e, const struct dtp_i2c_io *io)
{
    uint32_t i;
    struct dtp_i2c_io saved = {0};
    if (io) saved = *io;
    /* 再初期化は呼出側が機器の静止・回復を検証した後だけに限定する。 */
    uint8_t *bytes = (uint8_t *)e;
    for (i = 0; i < sizeof(*e); ++i) bytes[i] = 0;
    e->io = saved;
    e->phase = DTP_I2C_IDLE;
}

static enum dtp_i2c_result fail(struct dtp_i2c_engine *e, enum dtp_i2c_result result, int may_stop)
{
    if (may_stop) {
        /* 観察した制御bitを落とす停止要求。物理STOP/回復完了とは扱わない。 */
        control(e, 0x10, 0);
        e->stop_requested = 1;
    }
    e->phase = DTP_I2C_FAILED;
    e->result = result;
    e->requires_reset = 1;
    return result;
}

static void stop(struct dtp_i2c_engine *e)
{
    control(e, 0x10, 0);
    e->stop_requested = 1;
    e->phase = DTP_I2C_STOP;
}

static void receive_next(struct dtp_i2c_engine *e)
{
    /* 参考コードの最終byte/途中byteの制御差を表現する。ACK波形は未検証。 */
    control(e, 0x09, (e->rx_size - e->rx_completed > 1) ? 0x08 : 0);
    e->phase = DTP_I2C_RX;
}

static void transmit_next(struct dtp_i2c_engine *e)
{
    if (e->tx_submitted < e->tx_size) {
        e->io.write32(e->io.opaque, DTP_I2C_DATA, e->tx[e->tx_submitted++]);
        e->in_flight = 1;
        control(e, 0x01, 0);
        e->phase = DTP_I2C_TX;
    } else if (e->rx_size) {
        /* STOPを挟まず読み取りアドレスへ進む。実機でrepeated STARTを確認する。 */
        e->io.write32(e->io.opaque, DTP_I2C_DATA, ((uint32_t)e->address << 1) | 1);
        control(e, 0, 0x20);
        e->phase = DTP_I2C_ADDRESS_RX;
    } else {
        stop(e);
    }
}

enum dtp_i2c_result dtp_i2c_start(struct dtp_i2c_engine *e, uint32_t address,
    const uint8_t *tx, uint32_t tx_size, uint32_t rx_size, uint32_t now_us, uint32_t timeout_us)
{
    uint32_t i;
    if (!e || address > 0x7f || tx_size > DTP_I2C_MAX || rx_size > DTP_I2C_MAX ||
        (!tx_size && !rx_size) || (tx_size && !tx) || !timeout_us || timeout_us > 0x7fffffffu)
        return DTP_I2C_INVALID;
    if (e->requires_reset) return DTP_I2C_REOPEN_REQUIRED;
    if (e->phase != DTP_I2C_IDLE && e->phase != DTP_I2C_DONE) return DTP_I2C_BUSY;
    if (!e->io.read32 || !e->io.write32 || !e->io.owned || !e->io.owned(e->io.opaque))
        return DTP_I2C_NOT_READY;
    e->state = e->io.read32(e->io.opaque, DTP_I2C_STATE);
    e->control = e->io.read32(e->io.opaque, DTP_I2C_CONTROL);
    /* 他の処理が所有する可能性のあるバスを、この入口で回復/再試行しない。 */
    if (e->state & 0x80) return DTP_I2C_BUSY;
    if (e->control & 0x80) {
        e->tx_submitted = e->tx_completed = e->rx_completed = e->in_flight = e->stop_requested = 0;
        return fail(e, DTP_I2C_CONTROLLER_ERROR, 0);
    }
    e->address = (uint8_t)address;
    e->tx_size = (uint8_t)tx_size;
    e->rx_size = (uint8_t)rx_size;
    e->tx_submitted = e->tx_completed = e->rx_completed = e->in_flight = e->stop_requested = 0;
    for (i = 0; i < DTP_I2C_MAX; ++i) {
        e->tx[i] = i < tx_size ? tx[i] : 0;
        e->rx[i] = 0;
    }
    e->started_us = now_us;
    e->timeout_us = timeout_us;
    e->result = DTP_I2C_PENDING;
    e->io.write32(e->io.opaque, DTP_I2C_DATA, ((uint32_t)address << 1) | (tx_size ? 0 : 1));
    control(e, 0, 0x10);
    e->phase = tx_size ? DTP_I2C_ADDRESS_TX : DTP_I2C_ADDRESS_RX;
    return e->result;
}

enum dtp_i2c_result dtp_i2c_poll(struct dtp_i2c_engine *e, uint32_t now_us, int completion_ready)
{
    if (!e) return DTP_I2C_INVALID;
    if (e->phase == DTP_I2C_FAILED || e->phase == DTP_I2C_DONE) return e->result;
    if (e->phase == DTP_I2C_IDLE) return DTP_I2C_NOT_READY;
    /* 所有権喪失後はMMIOを追加発行せず、機器全体を再構築する。 */
    if (!e->io.owned(e->io.opaque)) return fail(e, DTP_I2C_OWNERSHIP_LOST, 0);
    e->state = e->io.read32(e->io.opaque, DTP_I2C_STATE);
    e->control = e->io.read32(e->io.opaque, DTP_I2C_CONTROL);
    /* uint32差分でwrapを扱い、pollごとに期限を延長しない。 */
    if ((uint32_t)(now_us - e->started_us) >= e->timeout_us)
        return fail(e, DTP_I2C_TIMEOUT, 1);
    if ((e->control & 0x80) ||
        ((e->phase == DTP_I2C_ADDRESS_TX || e->phase == DTP_I2C_ADDRESS_RX) && (e->state & 0x10)))
        return fail(e, DTP_I2C_CONTROLLER_ERROR, 1);
    /* 完了/進行通知の物理条件はHALの未解決部分。仮の時間待ちで代用しない。 */
    if (!completion_ready && e->phase != DTP_I2C_STOP) return DTP_I2C_PENDING;
    switch (e->phase) {
    case DTP_I2C_ADDRESS_TX:
        transmit_next(e);
        break;
    case DTP_I2C_TX:
        e->tx_completed = e->tx_submitted;
        e->in_flight = 0;
        transmit_next(e);
        break;
    case DTP_I2C_ADDRESS_RX:
        receive_next(e);
        break;
    case DTP_I2C_RX:
        e->rx[e->rx_completed++] = (uint8_t)e->io.read32(e->io.opaque, DTP_I2C_DATA);
        if (e->rx_completed == e->rx_size) stop(e);
        else receive_next(e);
        break;
    case DTP_I2C_STOP:
        if (!(e->state & 0x80)) {
            e->phase = DTP_I2C_DONE;
            e->result = DTP_I2C_OK;
        }
        break;
    default:
        return fail(e, DTP_I2C_INVALID, 1);
    }
    return e->result;
}
