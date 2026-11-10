/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t read_reg(uint32_t address);
static void write_reg(uint32_t address, uint32_t value);
#define XZ_I2C_READ(address) read_reg(address)
#define XZ_I2C_WRITE(address, value) write_reg(address, value)
#include "relay.c"

static unsigned writes, starts, stops, received, control;
static unsigned nack, bus_error, byte_timeout, stuck_after_stop;
static unsigned sent[16], sent_count;
static unsigned divider = 0x3c;

static uint32_t read_reg(uint32_t address)
{
    switch (address) {
    case I2C_STATUS: return (nack ? 0x10u : 0) | (stops && stuck_after_stop ? 0x80u : 0);
    case I2C_CONTROL: return (control & ~0x32u) | (bus_error ? 0x80u : 0) | !byte_timeout;
    case I2C_DATA: return 0xa0 + received++;
    case I2C_DIVIDER: return divider;
    default: assert(0); return 0;
    }
}

static void write_reg(uint32_t address, uint32_t value)
{
    ++writes;
    if (address == I2C_CONTROL) {
        control = value;
        starts += value == 0x52;
        stops += value == 0x42;
    } else {
        assert(address == I2C_DATA && sent_count < sizeof(sent) / sizeof(*sent));
        sent[sent_count++] = value;
    }
}

static void request(uint32_t packet[16], unsigned op, const uint8_t *data, unsigned size)
{
    memset(packet, 0, 64);
    packet[0] = XZ_MAGIC;
    packet[1] = 7;
    packet[2] = op;
    packet[3] = size;
    if (size) memcpy(packet + 6, data, size);
}

static void expect_failure(unsigned result)
{
    const uint8_t batch[] = {0x11, 1, 2, 0x80};
    uint32_t packet[16];
    request(packet, XZ_BATCH, batch, sizeof(batch));
    unsigned previous_stops = stops;
    xz_i2c_relay(packet);
    assert(packet[3] == result && packet[4] == 0 && packet[5] == 0);
    assert(stops == previous_stops + 1);
}

int main(void)
{
    uint32_t packet[16];
    request(packet, XZ_PROBE, 0, 0);
    xz_i2c_relay(packet);
    assert(packet[2] == (XZ_REPLY | XZ_PROBE) && packet[3] == XZ_OK);
    assert(packet[5] == 12 && packet[8] == 0x3c && !writes);

    const uint8_t malformed[] = {0x11, 1, 0, 0x80, 0x11, 8, 0};
    request(packet, XZ_BATCH, malformed, sizeof(malformed));
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_INVALID && !writes);

    const uint8_t batch[] = {0x11, 1, 2, 0x80, 0x10, 2, 0, 0x05, 0x12};
    const unsigned expected[] = {0x22, 0x80, 0x23, 0x20, 0x05, 0x12};
    request(packet, XZ_BATCH, batch, sizeof(batch));
    divider = 0;
    xz_i2c_relay(packet);
    assert(packet[3] == XZ_NOT_READY && !writes);
    divider = 0x3c;
    request(packet, XZ_BATCH, batch, sizeof(batch));
    xz_i2c_relay(packet);
    const uint8_t *reply = (const uint8_t *)(packet + 6);
    assert(packet[1] == 7 && packet[2] == (XZ_REPLY | XZ_BATCH));
    assert(packet[3] == XZ_OK && packet[4] == 2 && packet[5] == 2);
    assert(reply[0] == 0xa0 && reply[1] == 0xa1 && reply[2] == 0);
    assert(starts == 2 && stops == 2 && sent_count == sizeof(expected) / sizeof(*expected));
    assert(!memcmp(sent, expected, sizeof(expected)));
    const unsigned previous_writes = writes;
    xz_i2c_relay(packet);
    assert(writes == previous_writes);

    nack = 1;
    expect_failure(XZ_NACK);
    nack = 0;
    bus_error = 1;
    expect_failure(XZ_BUS_ERROR);
    bus_error = 0;
    byte_timeout = 1;
    expect_failure(XZ_TIMEOUT);
    byte_timeout = 0;
    stuck_after_stop = 1;
    stops = 0;
    expect_failure(XZ_STOP_TIMEOUT);
    return 0;
}
