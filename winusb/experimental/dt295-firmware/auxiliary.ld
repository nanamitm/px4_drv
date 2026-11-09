/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-fr30")
OUTPUT_ARCH(fr30)
ENTRY(dtp_aux_entry)
PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x000a0000;
    .text : { KEEP(*(.startup)) *(.text*) } :code
    .rodata : { *(.rodata*) } :code
    .data : { *(.data*) } :data
    ASSERT(. <= 0x000a2000, "auxiliary code exceeds development window")
    . = ALIGN(4);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss*) *(COMMON) . = ALIGN(4); } :data
    __bss_end = .;
    ASSERT(. <= 0x000a3000, "auxiliary BSS overlaps reserved stack")
    __stack_top = 0x000a37fc;
    /* 0xa3900..0xa393fはPAF1メールボックスで、ロード対象セグメントに含めない。 */
}
