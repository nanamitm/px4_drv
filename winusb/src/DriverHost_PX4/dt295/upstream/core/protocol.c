/* SPDX-License-Identifier: MIT */
#include "mb86H/protocol.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

uint32_t mb86H_debug_u32be_get(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

void mb86H_debug_u32be_put(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

void mb86H_debug_packet_pack(uint8_t packet[MB86H_DEBUG_PACKET_SIZE], uint32_t magic,
                           uint32_t sequence, uint32_t operation,
                           uint32_t address, uint32_t argument, uint32_t budget,
                           const uint8_t data[MB86H_DEBUG_PACKET_DATA_SIZE])
{
    memset(packet, 0, MB86H_DEBUG_PACKET_SIZE);
    mb86H_debug_u32be_put(packet + MB86H_DEBUG_PACKET_MAGIC_OFFSET, magic);
    mb86H_debug_u32be_put(packet + MB86H_DEBUG_PACKET_SEQUENCE_OFFSET, sequence);
    mb86H_debug_u32be_put(packet + MB86H_DEBUG_PACKET_OPERATION_OFFSET, operation);
    mb86H_debug_u32be_put(packet + MB86H_DEBUG_PACKET_ADDRESS_OFFSET, address);
    mb86H_debug_u32be_put(packet + MB86H_DEBUG_PACKET_ARGUMENT_OFFSET, argument);
    mb86H_debug_u32be_put(packet + MB86H_DEBUG_PACKET_BUDGET_OFFSET, budget);
    if (data)
        memcpy(packet + MB86H_DEBUG_PACKET_DATA_OFFSET, data,
               MB86H_DEBUG_PACKET_DATA_SIZE);
}

int mb86H_debug_response_matches(const uint8_t response[MB86H_DEBUG_PACKET_SIZE],
                               uint32_t magic, uint32_t sequence)
{
    return mb86H_debug_u32be_get(response + MB86H_DEBUG_PACKET_MAGIC_OFFSET) == magic &&
           mb86H_debug_u32be_get(response + MB86H_DEBUG_PACKET_SEQUENCE_OFFSET) == sequence;
}
