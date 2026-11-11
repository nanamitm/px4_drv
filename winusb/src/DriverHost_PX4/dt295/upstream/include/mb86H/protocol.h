/* SPDX-License-Identifier: MIT */
#ifndef MB86H_DEBUG_PROTOCOL_CORE_H
#define MB86H_DEBUG_PROTOCOL_CORE_H

#include "mb86H/protocol/constants.h"
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

uint32_t mb86H_debug_u32be_get(const uint8_t *bytes);
void mb86H_debug_u32be_put(uint8_t *bytes, uint32_t value);
void mb86H_debug_packet_pack(uint8_t packet[MB86H_DEBUG_PACKET_SIZE], uint32_t magic,
                           uint32_t sequence, uint32_t operation,
                           uint32_t address, uint32_t argument, uint32_t budget,
                           const uint8_t data[MB86H_DEBUG_PACKET_DATA_SIZE]);
int mb86H_debug_response_matches(const uint8_t response[MB86H_DEBUG_PACKET_SIZE],
                               uint32_t magic, uint32_t sequence);

#endif
