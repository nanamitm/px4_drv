/* SPDX-License-Identifier: MIT */
/* Chip register addresses shared by the USB and PCIe paths.  Each address is
 * one physical register; the names follow the first user and the comment
 * lists the other call sites so the aliases stay discoverable. */
#ifndef MB86H_REGS_H
#define MB86H_REGS_H

/* Boot state word: pcie boot.c BOOT_EVENT, USB pix_dt295 STATE, gv_mvp_xz rawstate. */
#define MB86H_REG_BOOT_STATE 0x82008u

/* Boot ready/IRQ word: pcie boot.c BOOT_IRQ, secondary.c, USB pix_dt295 READY. */
#define MB86H_REG_BOOT_IRQ 0x90074u

/* Shared DMA/RX status and mask pair: boot_dma.c BOOT_STATUS, sniff.c and
 * src/receive.c RX_STATUS/STATUS, plus the MASK counterpart. */
#define MB86H_REG_RX_STATUS 0x1e010cu
#define MB86H_REG_RX_MASK 0x1e0110u

#endif
