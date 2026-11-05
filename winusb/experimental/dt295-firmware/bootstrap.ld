/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-fr30")
OUTPUT_ARCH(fr30)
ENTRY(dtp_usb_hook)
SECTIONS
{
    . = 0x000e1000;
    .text : { *(.text*) }
    ASSERT(SIZEOF(.text) <= 0x800, "bootstrap exceeds reserved development window")
    . = 0x000ea6fc;
    .hook : { *(.hook) }
    ASSERT(SIZEOF(.hook) == 8, "DT295 boot hook must be exactly 8 bytes")
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "bootstrap must not require data initialization")
}
