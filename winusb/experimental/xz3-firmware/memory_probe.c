/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GV-MVP/XZ3のTSリング周辺のSDRAMを読む診断用の中継。
 * boot0のUSBメモリ要求は24ビットの番地しか指定できず、FR30がTSリングを読む0x80000000以降に届かない。
 * 受信用の中継 (ts_probe.c) はboot0の本体の窓をほぼ使い切るため、受信を終えてboot RAMを戻した後に、
 * sdram_probe.pyがこの中継を同じ窓へ読み込んで使う。読み出しだけで、SDRAMへは書き込まない。
 *
 * 操作14: word 4の番地から32バイトを読み、word 6〜13へ返す。
 * 番地は4バイト境界で、0x80000000〜0x83ffffffに限る。範囲外は状態1を返し、メモリへアクセスしない。
 */
#include <stdint.h>

#define REG(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define MAGIC 0x585a4931u
#define SDRAM 0x80000000u
#define SDRAM_LIMIT 0x84000000u

void xz_i2c_relay(uint32_t packet[16])
{
    if (packet[0] != MAGIC || packet[2] != 14 || packet[3] || packet[5])
        return;
    const uint32_t address = packet[4];
    packet[2] |= 0x80000000u;
    packet[5] = 32;
    if (address & 3u || address < SDRAM || address > SDRAM_LIMIT - 32) {
        packet[3] = 1;
        for (unsigned i = 6; i < 16; ++i)
            packet[i] = 0;
        return;
    }
    packet[3] = 0;
    for (unsigned i = 0; i < 8; ++i)
        packet[6 + i] = REG(address + i * 4);
    packet[14] = 0;
    packet[15] = 0;
}
