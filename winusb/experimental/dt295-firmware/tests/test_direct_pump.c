/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t *mmio(uint32_t address);
static void fifo_write(uint32_t word);
static uint32_t i2c_read(uint32_t address);
static void i2c_write(uint32_t address, uint32_t value);
#define DTP_MMIO(address) (*mmio(address))
#define DTP_FIFO_WRITE(word) fifo_write(word)
#define DTP_I2C_READ(address) i2c_read(address)
#define DTP_I2C_WRITE(address, value) i2c_write(address, value)
#include "../direct_pump.c"

static uint32_t ring[RING_SIZE / 4];
static uint32_t producer, limit, select_reg, fifo_status, run = 0x02000000;
static uint8_t output[512 * 512];
static unsigned written;
static unsigned i2c_writes, i2c_stops, i2c_reads;
static uint32_t control, nack;

static uint32_t *mmio(uint32_t address)
{
    if (address >= RING_BASE && address < RING_BASE + RING_SIZE)
        return &ring[(address - RING_BASE) / 4];
    switch (address) {
    case PRODUCER: return &producer;
    case LIMIT: return &limit;
    case SELECTOR: return &select_reg;
    case FIFO_STATUS: return &fifo_status;
    case 0x01902010: return &run;
    default: assert(0); return NULL;
    }
}

static void fifo_write(uint32_t word)
{
    assert(written + 4 <= sizeof(output));
    for (unsigned i = 0; i < 4; ++i)
        output[written++] = (uint8_t)(word >> (24 - i * 8));
}

static uint32_t i2c_read(uint32_t address)
{
    switch (address) {
    case I2C_CONTROL: return control | 1;
    case I2C_STATUS: return nack;
    case I2C_DATA: return 0xa0 + i2c_reads++;
    default: assert(0); return 0;
    }
}

static void i2c_write(uint32_t address, uint32_t value)
{
    ++i2c_writes;
    if (address == I2C_CONTROL) {
        control = value;
        i2c_stops += value == 0x42;
    } else {
        assert(address == I2C_DATA);
    }
}

static uint8_t payload_byte(unsigned position)
{
    return position % 188 ? (uint8_t)(0x80 + position % 63) : 0x47;
}

static void populate(unsigned offset)
{
    memset(ring, 0, sizeof(ring));
    for (unsigned record = 0; record < RING_SIZE / 192; ++record) {
        for (unsigned b = 0; b < 192; ++b) {
            unsigned addr = (offset + record * 192 + b) % RING_SIZE;
            uint8_t value = b < 188 ? payload_byte(record * 188 + b) : 0xfe;
            ring[addr / 4] |= (uint32_t)value << (24 - (addr % 4) * 8);
        }
    }
}

static void compact_all_offsets(void)
{
    const unsigned payload_size = RING_SIZE / 192 * 188;
    for (unsigned offset = 0; offset < 192; ++offset) {
        populate(offset);
        uint32_t cursor = 0, state = UNALIGNED;
        unsigned delivered = 0;
        for (unsigned n = 0; n < 4; ++n) {
            uint32_t packet[16] = {0};
            packet[2] = 4;
            packet[6] = cursor;
            packet[7] = 100;
            packet[8] = state;
            producer = (cursor + 0x10000) % RING_SIZE;
            fifo_status = 4;
            select_reg = 15;
            written = 0;
            dtp_bulk_pump(packet);
            assert(packet[2] == 0x80000004 && packet[5] == 24 && !packet[10]);
            assert(packet[7] == 100 && written == 100 * 512 && select_reg == 15);
            for (unsigned b = 0; b < written; ++b)
                assert(output[b] == payload_byte((delivered + b) % payload_size));
            delivered += written;
            cursor = packet[6];
            state = packet[11];
            assert((state >> 8) == offset && (state & 255) == delivered % 188);
            assert(cursor == (offset + delivered + 4 * (delivered / 188)) % RING_SIZE);
            assert(limit == cursor / 192 * 192);
        }
    }
}

static void sync_failure_is_atomic(void)
{
    populate(0);
    ring[192 / 4] &= 0x00ffffff;
    uint32_t packet[16] = {0};
    packet[2] = 4;
    packet[7] = 1;
    producer = 1920;
    fifo_status = 4;
    select_reg = 15;
    written = 0;
    dtp_bulk_pump(packet);
    assert(packet[10] == 3 && !packet[7] && !written);
}

static void i2c_batch(void)
{
    uint32_t packet[16] = {0};
    const uint8_t batch[] = {0x10, 2, 0, 0x0e, 0x77, 0x11, 1, 2, 0xbc};
    packet[2] = 3;
    packet[4] = sizeof(batch);
    memcpy(&packet[6], batch, sizeof(batch));
    dtp_bulk_pump(packet);
    const uint8_t *response = (const uint8_t *)&packet[8];
    assert(packet[2] == 0x80000003 && packet[5] == 10 && !packet[6] && packet[7] == 2);
    assert(response[0] == 0xa0 && response[1] == 0xa1 && i2c_stops == 2);

    packet[2] = 3;
    packet[4] = sizeof(batch);
    memcpy(&packet[6], batch, sizeof(batch));
    ((uint8_t *)&packet[6])[5] = 0x12;
    unsigned before = i2c_writes;
    dtp_bulk_pump(packet);
    assert(packet[6] == 1 && !packet[7] && i2c_writes == before);

    packet[2] = 3;
    packet[4] = sizeof(batch);
    memcpy(&packet[6], batch, sizeof(batch));
    nack = 0x10;
    dtp_bulk_pump(packet);
    assert(packet[6] == 4 && !packet[7] && i2c_stops == 3);
}

int main(void)
{
    compact_all_offsets();
    sync_failure_is_atomic();
    i2c_batch();
    puts("PASS direct PIF1 TS compaction and I2C batch");
}
