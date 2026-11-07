/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-fr30")
OUTPUT_ARCH(fr30)
ENTRY(dtp_entry)
SECTIONS
{
    /* 解析で確認したRAM範囲。実機の配置可否はUSBアダプターと併せて検証する。 */
    . = 0x000e1100;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "resident core must not require BSS initialization")
    ASSERT(. <= 0x000e1800, "protocol core exceeds reserved development window")
}
