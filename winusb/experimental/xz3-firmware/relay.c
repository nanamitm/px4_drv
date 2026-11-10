/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

#ifndef XZ_I2C_READ
#define XZ_I2C_READ(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define XZ_I2C_WRITE(address, value) (XZ_I2C_READ(address) = (value))
#endif

#define I2C_STATUS 0x00fd8fe0u
#define I2C_CONTROL 0x00fd8fe4u
#define I2C_DIVIDER 0x00fd8fe8u
#define I2C_DATA 0x00fd8ff0u
#define I2C_POLLS 64000u

enum { XZ_OK, XZ_INVALID, XZ_TIMEOUT, XZ_BUS_ERROR, XZ_NACK, XZ_STOP_TIMEOUT, XZ_NOT_READY };
enum { XZ_PROBE = 1, XZ_BATCH };
#define XZ_MAGIC 0x585a4931u
#define XZ_REPLY 0x80000000u

static unsigned wait_idle(void)
{
    for (unsigned n = 0; n < I2C_POLLS; ++n)
        if (!(XZ_I2C_READ(I2C_STATUS) & 0x80))
            return XZ_OK;
    return XZ_TIMEOUT;
}

static unsigned wait_byte(uint32_t *control, unsigned *budget)
{
    while (*budget) {
        --*budget;
        *control = XZ_I2C_READ(I2C_CONTROL);
        if (*control & 0x80)
            return XZ_BUS_ERROR;
        if (*control & 1)
            return XZ_OK;
    }
    return XZ_TIMEOUT;
}

static unsigned send_byte(uint32_t data, uint32_t flags, uint32_t *control,
                          unsigned *budget)
{
    XZ_I2C_WRITE(I2C_DATA, data);
    XZ_I2C_WRITE(I2C_CONTROL, flags);
    unsigned error = wait_byte(control, budget);
    if (!error && (XZ_I2C_READ(I2C_STATUS) & 0x10))
        return XZ_NACK;
    return error;
}

/* DT295で実測したコントローラー手順。XZ側の給電・ピン設定は呼び出し側の責務。 */
static unsigned transfer(unsigned address, const uint8_t *tx, unsigned tx_size,
                         uint8_t *rx, unsigned rx_size)
{
    uint32_t control;
    unsigned budget = I2C_POLLS;
    if (wait_idle())
        return XZ_TIMEOUT;
    unsigned error = send_byte(address * 2 + !tx_size, 0x52, &control, &budget);
    for (unsigned i = 0; !error && i < tx_size; ++i)
        error = send_byte(tx[i], control & ~1u, &control, &budget);
    if (!error && tx_size && rx_size)
        error = send_byte(address * 2 + 1, (control & ~1u) | 0x20, &control, &budget);
    for (unsigned i = 0; !error && i < rx_size; ++i) {
        XZ_I2C_WRITE(I2C_CONTROL, (control & ~9u) | (i + 1 < rx_size ? 8u : 0u));
        error = wait_byte(&control, &budget);
        if (!error)
            rx[i] = (uint8_t)XZ_I2C_READ(I2C_DATA);
    }
    /* STARTを発行した場合は転送エラーでもSTOPを試みる。 */
    XZ_I2C_WRITE(I2C_CONTROL, 0x42);
    return wait_idle() ? XZ_STOP_TIMEOUT : error;
}

static unsigned validate(const uint8_t *payload, unsigned size)
{
    unsigned reads = 0;
    if (!size || size > 40)
        return XZ_INVALID;
    for (unsigned pos = 0; pos < size;) {
        if (size - pos < 3)
            return XZ_INVALID;
        unsigned address = payload[pos++], tx = payload[pos++], rx = payload[pos++];
        if (address < 0x08 || address > 0x77 || tx > 8 || rx > 8 ||
            (!tx && !rx) || tx > size - pos || rx > 40 - reads)
            return XZ_INVALID;
        pos += tx;
        reads += rx;
    }
    return XZ_OK;
}


void xz_i2c_relay(uint32_t packet[16])
{
    if (packet[0] != XZ_MAGIC || (packet[2] & XZ_REPLY))
        return;

    uint8_t *payload = (uint8_t *)&packet[6];
    uint8_t read[40];
    unsigned error = XZ_INVALID, count = 0, completed = 0;
    unsigned size = packet[3];
    if (!packet[4] && !packet[5]) {
        if (packet[2] == XZ_PROBE && !size) {
            /* 最初の実機試験ではSTARTを発行せず、レジスターの状態だけを返す。 */
            for (unsigned i = 6; i < 16; ++i)
                packet[i] = 0;
            packet[6] = XZ_I2C_READ(I2C_STATUS);
            packet[7] = XZ_I2C_READ(I2C_CONTROL);
            packet[8] = XZ_I2C_READ(I2C_DIVIDER);
            error = XZ_OK;
            count = 12;
            goto reply;
        }
        if (packet[2] == XZ_BATCH) {
            error = validate(payload, size);
            /* XZの既存初期化で設定される分周値を確認してからバスに触れる。 */
            if (!error && XZ_I2C_READ(I2C_DIVIDER) != 0x3c)
                error = XZ_NOT_READY;
        }
    }
    for (unsigned pos = 0; !error && pos < size; ++completed) {
        unsigned address = payload[pos++], tx = payload[pos++], rx = payload[pos++];
        error = transfer(address, payload + pos, tx, read + count, rx);
        if (error)
            break;
        pos += tx;
        count += rx;
    }
    if (error)
        count = 0;
    for (unsigned i = 0; i < 40; ++i)
        payload[i] = i < count ? read[i] : 0;
reply:
    packet[3] = error;
    packet[4] = completed;
    packet[5] = count;
    packet[2] |= XZ_REPLY;
}
