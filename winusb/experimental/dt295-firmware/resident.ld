/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-fr30")
OUTPUT_ARCH(fr30)
ENTRY(dtp_entry)
SECTIONS
{
    /* 解析で確認したRAM範囲。実機の配置可否はUSBアダプターと併せて検証する。 */
    . = DEFINED(dtp_direct_layout) ? 0x000e1060 : 0x000e1100;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "resident core must not require BSS initialization")
    /* 0x000e1000〜0x000e8fffはブートRAMのコードから参照がなく、電源投入直後は不定値だった。
       カードFIFOの一括転送を加えるため、使用範囲を0x000e2000まで広げている。 */
    ASSERT(. <= 0x000e2000, "protocol core exceeds reserved development window")
}
