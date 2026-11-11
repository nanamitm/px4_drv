/* SPDX-License-Identifier: GPL-2.0-only */
/* Controller sequence adapted from coax, Copyright 2026 Till0196.
 * See torne-hal.md for pinned source and license provenance. */
#include "torne_hal.h"

static int owned(void *opaque)
{
    struct dtp_torne_hal *h = opaque;
    if (h->fault != DTP_I2C_OK) return 0;
    if (!h->bus.exclusive(h->bus.opaque, h->generation)) {
        h->fault = DTP_I2C_OWNERSHIP_LOST;
        h->prepared = 0;
        return 0;
    }
    return 1;
}

static void io_fault(struct dtp_torne_hal *h, uint32_t address)
{
    h->fault = DTP_I2C_IO_ERROR;
    h->fault_address = address;
    h->prepared = 0;
}

static uint32_t read_at(struct dtp_torne_hal *h, uint32_t address)
{
    uint32_t value = 0;
    if (owned(h) && !h->bus.read32(h->bus.opaque, address, &value)) io_fault(h, address);
    return value;
}

static void write_at(struct dtp_torne_hal *h, uint32_t address, uint32_t value)
{
    if (owned(h) && !h->bus.write32(h->bus.opaque, address, value)) io_fault(h, address);
}

static uint32_t read_offset(void *opaque, uint32_t offset)
{
    return read_at(opaque, 0xfd8000u + offset);
}

static void write_offset(void *opaque, uint32_t offset, uint32_t value)
{
    write_at(opaque, 0xfd8000u + offset, value);
}

static enum dtp_i2c_result result(struct dtp_torne_hal *h, enum dtp_i2c_result r)
{
    if (h->fault != DTP_I2C_OK) {
        /* 失敗した読み書きを0値や成功として公開しない。以後のI/Oも停止する。 */
        h->engine.phase = DTP_I2C_FAILED;
        h->engine.requires_reset = 1;
        h->engine.result = h->fault;
        return h->fault;
    }
    return r;
}

void dtp_torne_init(struct dtp_torne_hal *h, const struct dtp_torne_bus *bus, uint32_t generation)
{
    struct dtp_torne_bus saved = {0};
    struct dtp_i2c_io io = {h, read_offset, write_offset, owned};
    uint32_t i;
    uint8_t *p = (uint8_t *)h;
    if (bus) saved = *bus;
    for (i = 0; i < sizeof(*h); ++i) p[i] = 0;
    h->bus = saved;
    h->generation = generation;
    if (!saved.read32 || !saved.write32 || !saved.write16 || !saved.now_us || !saved.exclusive)
        h->fault = DTP_I2C_INVALID;
    dtp_i2c_init(&h->engine, &io);
    h->engine.torne_controls = 1;
}

enum dtp_i2c_result dtp_torne_prepare(struct dtp_torne_hal *h)
{
    uint32_t value;
    if (!h) return DTP_I2C_INVALID;
    if (h->fault != DTP_I2C_OK) return h->fault;
    /* prepareの再呼出しで失敗や転送途中を暗黙回復しない。 */
    if (h->engine.requires_reset) return DTP_I2C_REOPEN_REQUIRED;
    if (h->prepared) return DTP_I2C_BUSY;
    if (!owned(h)) return result(h, h->fault);
    value = read_at(h, 0xfd8010);
    write_at(h, 0xfd8010, value | 0x10);
    value = read_at(h, 0xfd8014);
    write_at(h, 0xfd8014, value & ~0x10u);
    write_at(h, 0xfd80c0, 1);
    write_at(h, 0xfd80c0, 0);
    write_at(h, 0xfd80c4, 0);
    if (owned(h) && !h->bus.write16(h->bus.opaque, 0x90114, 0x8008)) io_fault(h, 0x90114);
    write_at(h, 0xfd8ff8, 6);
    write_at(h, 0xfd8ff4, 8);
    write_at(h, 0xfd8fe8, 0x3c);
    write_at(h, 0xfd8fe4, 0x40);
    /* バスの静止だけを確認する。自己消去bit等の未確認readbackを要求しない。 */
    value = read_at(h, 0xfd8fe0);
    if (h->fault != DTP_I2C_OK) return result(h, h->fault);
    if (value & 0x80) {
        h->engine.requires_reset = 1;
        return DTP_I2C_BUSY;
    }
    h->prepared = 1;
    return DTP_I2C_OK;
}

enum dtp_i2c_result dtp_torne_start(struct dtp_torne_hal *h, uint32_t address,
    const uint8_t *tx, uint32_t tx_size, uint32_t rx_size, uint32_t timeout_us)
{
    enum dtp_i2c_result r;
    if (!h) return DTP_I2C_INVALID;
    if (h->fault != DTP_I2C_OK) return h->fault;
    if (!h->prepared) return DTP_I2C_NOT_READY;
    r = dtp_i2c_start(&h->engine, address, tx, tx_size, rx_size,
        h->bus.now_us(h->bus.opaque), timeout_us);
    return result(h, r);
}

enum dtp_i2c_result dtp_torne_poll(struct dtp_torne_hal *h)
{
    enum dtp_i2c_result r;
    if (!h) return DTP_I2C_INVALID;
    if (h->fault != DTP_I2C_OK) return h->fault;
    if (!h->prepared) return DTP_I2C_NOT_READY;
    r = dtp_i2c_poll(&h->engine, h->bus.now_us(h->bus.opaque), -1);
    return result(h, r);
}
