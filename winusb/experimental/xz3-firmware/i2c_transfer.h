/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XZ_I2C_TRANSFER_H
#define XZ_I2C_TRANSFER_H

#include <stdint.h>

#ifndef XZ_I2C_READ
#define XZ_I2C_READ(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define XZ_I2C_WRITE(address, value) (XZ_I2C_READ(address) = (value))
#define XZ_MMIO_READ16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define XZ_MMIO_WRITE16(address, value) (XZ_MMIO_READ16(address) = (value))
#endif

#define I2C_STATUS 0x00fd8fe0u
#define I2C_CONTROL 0x00fd8fe4u
#define I2C_DIVIDER 0x00fd8fe8u
#define I2C_DATA 0x00fd8ff0u
#ifndef I2C_POLLS
#define I2C_POLLS 64000u
#endif

enum {
    XZ_OK, XZ_INVALID, XZ_TIMEOUT, XZ_BUS_ERROR, XZ_NACK, XZ_STOP_TIMEOUT, XZ_NOT_READY,
    XZ_VERIFY_FAILED,
};
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

/*
 * CONTROLへ書く値は、公開mAgicVISIONファームウェアのXZ常駐部の転送関数
 * (0x8038eb98) と同じ固定値を使う。DT295で実測した値 (0x52など) はbit 1を
 * 含み、GV-MVP/XZ3ではバイト完了が通知されなかった。XZ側の給電・ピン設定は
 * 呼び出し側の責務。
 */
#define I2C_CONTROL_BYTE 0x50u
#define I2C_CONTROL_ACK 0x08u
#define I2C_CONTROL_RESTART 0x70u
#define I2C_CONTROL_STOP 0x40u

static unsigned transfer(unsigned address, const uint8_t *tx, unsigned tx_size,
                         uint8_t *rx, unsigned rx_size)
{
    uint32_t control;
    unsigned budget = I2C_POLLS;
    if (wait_idle())
        return XZ_TIMEOUT;
    unsigned error = send_byte(address * 2 + !tx_size, I2C_CONTROL_BYTE, &control, &budget);
    for (unsigned i = 0; !error && i < tx_size; ++i)
        error = send_byte(tx[i], I2C_CONTROL_BYTE, &control, &budget);
    if (!error && tx_size && rx_size)
        error = send_byte(address * 2 + 1, I2C_CONTROL_RESTART, &control, &budget);
    for (unsigned i = 0; !error && i < rx_size; ++i) {
        /* 最後のバイト以外はACKを返し、最後のバイトはNACKで受信を終える。 */
        XZ_I2C_WRITE(I2C_CONTROL, I2C_CONTROL_BYTE | (i + 1 < rx_size ? I2C_CONTROL_ACK : 0u));
        error = wait_byte(&control, &budget);
        if (!error)
            rx[i] = (uint8_t)XZ_I2C_READ(I2C_DATA);
    }
    /* STARTを発行した場合は転送エラーでもSTOPを試みる。 */
    XZ_I2C_WRITE(I2C_CONTROL, I2C_CONTROL_STOP);
    return wait_idle() ? XZ_STOP_TIMEOUT : error;
}

#endif
