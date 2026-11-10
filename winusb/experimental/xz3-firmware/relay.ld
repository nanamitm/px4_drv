/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-fr30")
OUTPUT_ARCH(fr30)
ENTRY(xz_usb_entry)
SECTIONS
{
    /* XZのブートストラップ本体用領域。ロード時の上書き範囲は別途検証する。 */
    . = 0x000ea61c;
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) *(.probe_state) . = ALIGN(4); }
    .hook 0x000e9a10 : { KEEP(*(.hook)) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.hook) == 8, "XZ hook must replace exactly two words")
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "relay must be stateless")
    ASSERT(SIZEOF(.text) <= 2236, "relay exceeds XZ boot0 body window")
}
