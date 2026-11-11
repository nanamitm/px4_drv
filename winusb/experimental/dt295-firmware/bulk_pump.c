/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded PIF1 diagnostic. The host owns TS setup, Bulk IN and cleanup. */
#include <stdint.h>
#define WORD(address) (*(volatile uint32_t *)(uintptr_t)(address))
/* direct_pump.cとDriverHostのsource_firmware.cppのRingSizeと一致させる。 */
#define RING_SIZE 0x600000u
#define RING_BASE 0x8138ec00u
#define PRODUCER 0x01902028u
#define LIMIT 0x0190202cu
#define FIFO_STATUS 0x00fdbd0cu
#define FIFO_DATA 0x00fdbd84u
#define SELECTOR 0x00fd814cu

void dtp_bulk_pump(uint32_t *packet)
{
    uint32_t cursor = packet[6], budget = packet[7];
    uint32_t sent = 0, result = 0, producer = 0;
    if (cursor >= RING_SIZE || (cursor & 511u) || !budget || budget > 256u ||
        WORD(0x01902010u) != 0x02000000u || WORD(0x01902020u) != (RING_BASE & 0x7fffffffu) ||
        WORD(0x01902024u) != RING_SIZE || WORD(SELECTOR) != 15u) {
        result = 1;
    } else {
        uint32_t phase = cursor;
        while (phase >= 192u) phase -= 192u;
        for (uint32_t tries = 0; tries < 64000u && sent < budget; ++tries) {
            producer = WORD(PRODUCER);
            if (producer >= RING_SIZE) {
                result = 2;
                break;
            }
            uint32_t available = producer >= cursor ? producer - cursor : RING_SIZE - cursor + producer;
            if (available < 512u || !(WORD(FIFO_STATUS) & 4u)) continue;
            uint32_t address = RING_BASE + cursor;
            for (uint32_t i = 0; i < 512u; i += 4u) WORD(FIFO_DATA) = WORD(address+i);
            WORD(SELECTOR) = 8;
            WORD(FIFO_STATUS) = 4;
            WORD(SELECTOR) = 15;
            cursor += 512u;
            if (cursor == RING_SIZE) cursor = 0;
            phase += 128u;
            if (phase >= 192u) phase -= 192u;
            WORD(LIMIT) = cursor - phase;
            ++sent;
        }
    }
    packet[2] |= 0x80000000u;
    packet[3] = 0;
    packet[4] = 1;
    packet[5] = 20;
    packet[6] = cursor;
    packet[7] = sent;
    packet[8] = producer;
    packet[9] = WORD(FIFO_STATUS);
    packet[10] = result;
    for (unsigned i = 11; i < 16; ++i) packet[i] = 0;
}
