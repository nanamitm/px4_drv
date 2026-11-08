/* SPDX-License-Identifier: GPL-2.0-only */
/* Main-CPU PIF1 direct I2C and 188-byte TS transport. */
#include <stdint.h>

#ifndef DTP_MMIO
#define DTP_MMIO(address) (*(volatile uint32_t *)(uintptr_t)(address))
#endif

#ifndef DTP_FIFO_WRITE
#define DTP_FIFO_WRITE(value) (DTP_MMIO(FIFO_DATA) = (value))
#endif
#ifndef DTP_I2C_READ
#define DTP_I2C_READ(address) DTP_MMIO(address)
#define DTP_I2C_WRITE(address, value) (DTP_MMIO(address) = (value))
#endif

#define RING_SIZE 0x30000u
#define RING_BASE 0x8138ec00u
#define PRODUCER 0x01902028u
#define LIMIT 0x0190202cu
#define FIFO_STATUS 0x00fdbd0cu
#define FIFO_DATA 0x00fdbd84u
#define SELECTOR 0x00fd814cu
#define I2C_STATUS 0x00fd8fe0u
#define I2C_CONTROL 0x00fd8fe4u
#define I2C_DATA 0x00fd8ff0u
#define UNALIGNED 0xffffffffu

static unsigned advance(unsigned cursor, unsigned bytes)
{
    cursor += bytes;
    return cursor < RING_SIZE ? cursor : cursor - RING_SIZE;
}

static unsigned available(unsigned from, unsigned to)
{
    return to >= from ? to - from : RING_SIZE - from + to;
}

static uint32_t ring_word(unsigned cursor)
{
    unsigned aligned = cursor & ~3u;
    uint32_t first = DTP_MMIO(RING_BASE + aligned);
    unsigned shift = (cursor & 3u) * 8;
    if (!shift)
        return first;
    return (first << shift) | (DTP_MMIO(RING_BASE + advance(aligned, 4)) >> (32 - shift));
}

static unsigned sync_at(unsigned cursor)
{
    return ring_word(cursor) >> 24 == 0x47;
}

/* 1 USBパケットぶんの全レコードを確認してからFIFOへ書く。 */
static unsigned block(unsigned cursor, unsigned phase)
{
    unsigned first = phase ? 188 - phase : 0;
    unsigned raw = phase ? first + 4 : 0;
    for (; first < 512; first += 188, raw += 192)
        if (!sync_at(advance(cursor, raw)))
            return 0;
    return 1;
}

__attribute__((noinline)) static void pump(uint32_t *packet)
{
    unsigned cursor = packet[6], budget = packet[7], state = packet[8];
    unsigned phase = state & 255, offset = state >> 8;
    unsigned sent = 0, error = 0, producer = 0;
    if (cursor >= RING_SIZE || !budget || budget > 512 ||
        (state != UNALIGNED && (phase >= 188 || (phase & 3) || offset >= 192)) ||
        (state == UNALIGNED && cursor != 0) ||
        DTP_MMIO(0x01902010u) != 0x02000000u ||
        DTP_MMIO(SELECTOR) != 15) {
        error = 1;
        goto reply;
    }
    for (unsigned polls = 0; polls < 64000 && sent < budget; ++polls) {
        producer = DTP_MMIO(PRODUCER);
        if (producer >= RING_SIZE) {
            error = 2;
            break;
        }
        unsigned pending = available(cursor, producer);
        if (state == UNALIGNED) {
            if (pending < 3 * 192)
                continue;
            for (offset = 0; offset < 192; ++offset)
                if (sync_at(offset) && sync_at(offset + 192) && sync_at(offset + 384))
                    break;
            if (offset == 192) {
                error = 3;
                break;
            }
            cursor = offset;
            phase = 0;
            state = offset << 8;
            pending -= offset;
        }
        /* 512バイトのTSがまたぐ最大3個の4バイト付加部と先読みの語を確保する。 */
        if (pending < 528 || !(DTP_MMIO(FIFO_STATUS) & 4))
            continue;
        if (!block(cursor, phase)) {
            error = 3;
            break;
        }
        for (unsigned word = 0; word < 128; ++word) {
            DTP_FIFO_WRITE(ring_word(cursor));
            phase += 4;
            if (phase == 188) {
                cursor = advance(cursor, 8);
                phase = 0;
            } else {
                cursor = advance(cursor, 4);
            }
        }
        DTP_MMIO(SELECTOR) = 8;
        DTP_MMIO(FIFO_STATUS) = 4;
        DTP_MMIO(SELECTOR) = 15;
        unsigned tail = offset + phase;
        if (tail >= 192)
            tail -= 192;
        DTP_MMIO(LIMIT) = cursor >= tail ? cursor - tail : RING_SIZE + cursor - tail;
        state = (offset << 8) | phase;
        ++sent;
    }
reply:
    packet[5] = 24;
    packet[6] = cursor;
    packet[7] = sent;
    packet[8] = producer;
    packet[9] = DTP_MMIO(FIFO_STATUS);
    packet[10] = error;
    packet[11] = state;
    for (unsigned i = 12; i < 16; ++i)
        packet[i] = 0;
}

static unsigned wait_byte(uint32_t *control)
{
    for (unsigned n = 0; n < 64000; ++n) {
        *control = DTP_I2C_READ(I2C_CONTROL);
        if (*control & 0x80)
            return 3;
        if (*control & 1)
            return 0;
    }
    return 2;
}

static unsigned ack(void)
{
    return DTP_I2C_READ(I2C_STATUS) & 0x10 ? 4 : 0;
}

__attribute__((noinline)) static unsigned transfer(unsigned address, const uint8_t *tx, unsigned tx_size,
                         uint8_t *rx, unsigned rx_size)
{
    uint32_t control = 0;
    unsigned polls = 0;
    while (DTP_I2C_READ(I2C_STATUS) & 0x80)
        if (++polls == 64000)
            return 2;

    DTP_I2C_WRITE(I2C_DATA, address * 2 + !tx_size);
    DTP_I2C_WRITE(I2C_CONTROL, 0x52);
    unsigned result = wait_byte(&control);
    if (!result)
        result = ack();
    for (unsigned i = 0; !result && i < tx_size; ++i) {
        DTP_I2C_WRITE(I2C_DATA, tx[i]);
        DTP_I2C_WRITE(I2C_CONTROL, control & ~1u);
        result = wait_byte(&control);
        if (!result)
            result = ack();
    }
    if (!result && tx_size && rx_size) {
        DTP_I2C_WRITE(I2C_DATA, address * 2 + 1);
        DTP_I2C_WRITE(I2C_CONTROL, (control & ~1u) | 0x20);
        result = wait_byte(&control);
        if (!result)
            result = ack();
    }
    for (unsigned i = 0; !result && i < rx_size; ++i) {
        DTP_I2C_WRITE(I2C_CONTROL, (control & ~9u) | (i + 1 < rx_size ? 8u : 0u));
        result = wait_byte(&control);
        if (!result)
            rx[i] = (uint8_t)DTP_I2C_READ(I2C_DATA);
    }
    DTP_I2C_WRITE(I2C_CONTROL, 0x42);
    polls = 0;
    while (DTP_I2C_READ(I2C_STATUS) & 0x80)
        if (++polls == 64000)
            return 2;
    return result;
}

__attribute__((noinline)) static void i2c(uint32_t *packet)
{
    uint8_t *payload = (uint8_t *)&packet[6], read[32];
    unsigned size = packet[4], at = 0, total = 0, error = 0, index = 0;
    if (!size || size > 40) {
        error = 1;
        goto reply;
    }
    /* 不正な操作列は先頭の転送から実行しない。 */
    while (at < size) {
        if (size - at < 3) {
            error = 1;
            goto reply;
        }
        unsigned address = payload[at++], tx = payload[at++], rx = payload[at++];
        if ((address != 0x10 && address != 0x11) || tx > 8 || rx > 8 ||
            (!tx && !rx) || tx > size - at || rx > sizeof(read) - total) {
            error = 1;
            goto reply;
        }
        total += rx;
        at += tx;
    }
    total = 0;
    for (at = 0; at < size; ++index) {
        unsigned address = payload[at++], tx = payload[at++], rx = payload[at++];
        error = transfer(address, payload + at, tx, read + total, rx);
        if (error)
            break;
        at += tx;
        total += rx;
    }
reply:
    if (error)
        total = 0;
    for (unsigned i = 0; i < 40; ++i)
        payload[i] = 0;
    packet[5] = 8 + total;
    packet[6] = error;
    packet[7] = index;
    for (unsigned i = 0; i < total; ++i)
        payload[8 + i] = read[i];
}

void dtp_bulk_pump(uint32_t *packet)
{
    if (packet[2] == 3)
        i2c(packet);
    else
        pump(packet);
    packet[2] |= 0x80000000u;
    packet[3] = 0;
    packet[4] = 1;
}
