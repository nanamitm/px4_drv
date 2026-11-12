/* SPDX-License-Identifier: GPL-2.0-only */

/* PX-S1UD Rio-A0 RAM-only probe for one USB bulk transfer. */

    .equ ROM_HIF_HOOK, 0x40208
    .equ ROM_ALLOC, 0x49dc
    .equ ROM_FREE, 0x4a08
    .equ ROM_COPY, 0x24a0
    .equ ROM_SEND_HIF, 0x9260
    .equ ROM_SEND_BULK, 0x9178
    .equ ROM_SET_COMPLETION, 0x12bec
    .equ SAMPLE_HEADER, 0xe5b400
    .equ SAMPLE_DESCRIPTOR, 0xe5b490
    .equ SAMPLE_PAYLOAD, 0xe20000

    .section .text
    .global _start
_start:
    push_s blink
    jl init_dma_buffers
    mov r0, usb_complete
    jl ROM_SET_COMPLETION
    mov r0, ROM_HIF_HOOK
    mov r1, handle_message
    st.di r1, [r0]
    pop_s blink
    mov_s r0, 1
    j_s [blink]

handle_message:
    ldh r1, [r0]
    breq r1, 668, version
    breq r1, 890, fixed_data
    mov_s r0, 0
    j_s [blink]

version:
    mov r1, version_frame
    mov_s r2, 56
    b send_reply
fixed_data:
    push_s blink
    push_s r13
    mov r13, r0
    ldh r1, [r0, 4]
    cmp r1, 12
    blo status
    ld r1, [r0, 8]
    breq r1, 0, status
    cmp r1, 2
    beq snapshot
    breq r1, 3, pll_readback
    breq r1, 4, pll_control_readback
    mov r0, stats
    ld r1, [r0, 20]
    brne r1, 0, status
    mov_s r1, 1
    st r1, [r0, 20]
    mov_s r0, 0
    mov_s r1, 1
    mov r2, SAMPLE_HEADER
    mov_s r3, 0
    jl ROM_SEND_BULK
    mov r1, stats
    breq r0, 0, rejected
    ld r2, [r1, 12]
    add_s r2, r2, 1
    st r2, [r1, 12]
    b status
rejected:
    st r0, [r1, 20]
    ld r2, [r1, 24]
    add_s r2, r2, 1
    st r2, [r1, 24]
status:
    mov r0, r13
    pop_s r13
    pop_s blink
    mov r1, stats
    mov_s r2, 28
    b send_reply

/* Exercise only the PLL frequency field. Restore the original word. */
pll_control_readback:
    mov r5, 0x820010
    ld.di r6, [r5]
    bic r7, r6, 2
    st.di r7, [r5]
    b pll_test
pll_readback:
    mov r5, 0
pll_test:
    mov r0, 0x852008
    ld.di r1, [r0]
    and r2, r1, 0x3ff
    or r2, r2, 0xdb6c5000
    st.di r2, [r0]
    ld.di r3, [r0]
    st.di r1, [r0]
    ld.di r4, [r0]
    mov r7, 0
    mov r8, 0
    breq r5, 0, pll_reply
    ld.di r7, [r5]
    st.di r6, [r5]
    ld.di r8, [r5]
pll_reply:
    mov r0, pll_result
    st r1, [r0, 8]
    st r2, [r0, 12]
    st r3, [r0, 16]
    st r4, [r0, 20]
    st r7, [r0, 24]
    st r8, [r0, 28]
    mov r0, r13
    pop_s r13
    pop_s blink
    mov r1, pll_result
    mov_s r2, 32
    b send_reply

snapshot:
    mov r1, rf_registers
    mov r2, rf_snapshot + 8
snapshot_next:
    ld.ab r3, [r1, 4]
    breq r3, 0, snapshot_done
    st.ab r3, [r2, 4]
    ld.di r3, [r3]
    st.ab r3, [r2, 4]
    b snapshot_next
snapshot_done:
    mov r0, r13
    pop_s r13
    pop_s blink
    mov r1, rf_snapshot
    mov r2, rf_snapshot_end - rf_snapshot
    b send_reply

usb_complete:
    mov r2, SAMPLE_HEADER
    brne r1, r2, ignore_completion
    mov r2, stats
    ld r3, [r2, 16]
    add_s r3, r3, 1
    st r3, [r2, 16]
    mov_s r3, 0
    st r3, [r2, 20]
ignore_completion:
    j_s [blink]

send_reply:
    push_s blink
    push_s r13
    push_s r14
    push_s r15
    st.aw r16, [sp, -4]
    mov r13, r0
    mov r14, r1
    mov r15, r2
    mov r0, r15
    jl ROM_ALLOC
    breq r0, 0, release_request
    mov r16, r0
    mov r1, r14
    mov r2, r15
    jl ROM_COPY
    ldb r0, [r13, 2]
    stb r0, [r16, 3]
    mov r0, r16
    jl ROM_SEND_HIF
release_request:
    mov r0, r13
    jl ROM_FREE
    ld.ab r16, [sp, 4]
    pop_s r15
    pop_s r14
    pop_s r13
    pop_s blink
    mov_s r0, 1
    j_s [blink]

    .balign 4
version_frame:
    .short 669
    .byte 11, 0
    .short 56, 0
    .short 0x2270
    .byte 0, 0, 0xfe, 0, 0, 1, 0, 0, 8, 1, 1, 10
    .asciz "PX-S1UD RAM PROBE"
    .space 16
stats:
    .short 891
    .byte 11, 0
    .short 28, 0
    /* Result, accepted, completed, busy, rejected. */
    .long 0, 0, 0, 0, 0

pll_result:
    .short 894
    .byte 11, 0
    .short 32, 0
    .long 0, 0, 0, 0, 0, 0

rf_registers:
    /* Fixed read-only diagnostic list. Never accept an address from USB. */
    .long 0x820010, 0x82005c, 0x820060, 0x8300f8
    .long 0x850018, 0x85001c, 0x852000, 0x852004, 0x852008, 0x85200c
    .long 0x852014, 0x852018, 0x85201c, 0x852020, 0x852024
    .long 0x85202c, 0x852030, 0x852034, 0x852040
    .long 0x856000, 0x856014, 0x856018, 0x856024
rf_registers_end:
    .long 0

rf_snapshot:
    .short 893
    .byte 11, 0
    .short rf_snapshot_end - rf_snapshot, 0
    .space (rf_registers_end - rf_registers) * 2
rf_snapshot_end:

init_dma_buffers:
    /* ROM bulk DMA uses this RAM region. Write it without the ARC cache. */
    mov r0, SAMPLE_HEADER
    mov r1, 0x0100037c
    st.di r1, [r0, 0]
    mov r1, 0x000d398c
    st.di r1, [r0, 4]
    mov r1, SAMPLE_DESCRIPTOR
    st.di r1, [r0, 8]

    mov r0, SAMPLE_DESCRIPTOR
    mov r1, SAMPLE_PAYLOAD
    st.di r1, [r0, 0]
    mov_s r1, 0
    st.di r1, [r0, 4]
    st.di r1, [r0, 8]
    mov r1, 14724
    st.di r1, [r0, 12]

    mov r0, SAMPLE_PAYLOAD
    mov r1, 0x5a5a5a5a
    mov r2, 3680
fill_payload:
    st.di.ab r1, [r0, 4]
    sub.f r2, r2, 1
    bne fill_payload
    mov_s r1, 0
    st.di r1, [r0]
    j_s [blink]
