/* SPDX-License-Identifier: GPL-2.0-only */

    .section .text
    .global _start
_start:
    /* Only touch memory inside the downloaded image. */
    mov r0, boot_marker
    mov r1, 0x53315544
    st.di r1, [r0]
    mov_s r0, 1
    j_s [blink]

    .org 0x40
boot_marker:
    .long 0
