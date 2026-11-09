/* SPDX-License-Identifier: GPL-2.0-only */

/* PX-S1UD Rio-A0 RAM-only probe for short 13ch/14ch I/Q captures. */

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
    mov r0, board_clock_config
    jl 0x19f0
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
    cmp r1, 4
    beq pll_control_readback
    cmp r1, 8
    beq capture_start
    cmp r1, 9
    beq capture_start
    cmp r1, 10
    beq capture_stop
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

/* Start a finite host-controlled capture. The host always sends stop. */
capture_start:
    mov r6, 0xdb6c5000
    cmp r1, 9
    mov.eq r6, 0xfb6c5000
    mov r0, 0x820000
    ld.di r1, [r0]
    bic r1, r1, 0x40004
    or r1, r1, 0xe02000
    st.di r1, [r0]
    mov r0, 0x820064
    ld.di r1, [r0]
    bic r1, r1, 1
    st.di r1, [r0]
    mov r0, 0x820068
    ld.di r1, [r0]
    bic r1, r1, 15
    st.di r1, [r0]
    mov r0, 0x82000c
    ld.di r1, [r0]
    bic r1, r1, 0x100
    st.di r1, [r0]
    mov r0, 0x820054
    ld.di r1, [r0]
    and r1, r1, 0x40
    st.di r1, [r0]
    mov r0, 0x820008
    ld.di r1, [r0]
    bic r1, r1, 0xffe000
    st.di r1, [r0]
    mov r0, rf_setup
capture_setup_next:
    ld.ab r1, [r0, 4]
    breq r1, 0, capture_frequency
    ld.ab r2, [r0, 4]
    st.di r2, [r1]
    b capture_setup_next
    .balign 4
board_clock_config:
    .byte 0xb0,0x04,0x08,0x03,0xc1,0x00,0x04,0x00,0xc1,0x00,0x00,0x02
    .byte 0x5a,0x00,0x03,0x00,0xb4,0x00,0xb2,0x25,0x01,0x01,0x02,0x01
    .byte 0x02,0x01,0xc1,0x01,0x01,0x01,0x02,0x0d,0x00,0x01,0x02,0x00

capture_frequency:
    jl pll_lock_sequence
    mov r0, 0x820064
    ld.di r1, [r0]
    bic r1, r1, 1
    st.di r1, [r0]
    mov r0, 0x820068
    ld.di r1, [r0]
    bic r1, r1, 15
    st.di r1, [r0]
    mov r0, 0x82000c
    ld.di r1, [r0]
    bic r1, r1, 0x100
    st.di r1, [r0]
    mov r0, 0x820054
    ld.di r1, [r0]
    and r1, r1, 0x40
    st.di r1, [r0]
    mov r0, 0x820008
    ld.di r1, [r0]
    bic r1, r1, 0xffe000
    st.di r1, [r0]
    mov r0, 10
    jl 0x1f44
    mov r2, route_trace + 0
    mov r3, 0x8407f4
    ld.di r4, [r3]
    st.di r4, [r2, 0]
    mov r3, 0x8407fc
    ld.di r4, [r3]
    st.di r4, [r2, 4]
    mov r3, 0x820064
    ld.di r4, [r3]
    st.di r4, [r2, 8]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 12]
    mov r0, 0x820064
    ld.di r1, [r0]
    or r1, r1, 1
    st.di r1, [r0]
    mov r2, route_trace + 16
    mov r3, 0x8407f4
    ld.di r4, [r3]
    st.di r4, [r2, 0]
    mov r3, 0x8407fc
    ld.di r4, [r3]
    st.di r4, [r2, 4]
    mov r3, 0x820064
    ld.di r4, [r3]
    st.di r4, [r2, 8]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 12]
    mov r0, 0x820068
    ld.di r1, [r0]
    or r1, r1, 12
    st.di r1, [r0]
    mov r2, route_trace + 32
    mov r3, 0x8407f4
    ld.di r4, [r3]
    st.di r4, [r2, 0]
    mov r3, 0x8407fc
    ld.di r4, [r3]
    st.di r4, [r2, 4]
    mov r3, 0x820068
    ld.di r4, [r3]
    st.di r4, [r2, 8]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 12]
    mov r0, 0x82000c
    ld.di r1, [r0]
    mov r1, 0x3ff
    st.di r1, [r0]
    mov r2, route_trace + 48
    mov r3, 0x8407f4
    ld.di r4, [r3]
    st.di r4, [r2, 0]
    mov r3, 0x8407fc
    ld.di r4, [r3]
    st.di r4, [r2, 4]
    mov r3, 0x82000c
    ld.di r4, [r3]
    st.di r4, [r2, 8]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 12]
    mov r0, 0x820054
    ld.di r1, [r0]
    or r1, r1, 0xffffffbf
    st.di r1, [r0]
    mov r2, route_trace + 64
    mov r3, 0x8407f4
    ld.di r4, [r3]
    st.di r4, [r2, 0]
    mov r3, 0x8407fc
    ld.di r4, [r3]
    st.di r4, [r2, 4]
    mov r3, 0x820054
    ld.di r4, [r3]
    st.di r4, [r2, 8]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 12]
    mov r0, 0x820008
    ld.di r1, [r0]
    or r1, r1, 0xffe000
    st.di r1, [r0]
    mov r2, route_trace + 80
    mov r3, 0x8407f4
    ld.di r4, [r3]
    st.di r4, [r2, 0]
    mov r3, 0x8407fc
    ld.di r4, [r3]
    st.di r4, [r2, 4]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 8]
    mov r3, 0x820008
    ld.di r4, [r3]
    st.di r4, [r2, 12]
    mov r0, 0x820010
    ld.di r1, [r0]
    or r1, r1, 4
    st.di r1, [r0]
    mov r0, 0x82005c
    mov r1, 0x01050006
    st.di r1, [r0]
    mov r1, 0x13
    st.di r1, [r0, 4]
    mov r0, 0x90003c
    ld.di r1, [r0]
    or r1, r1, 1
    st.di r1, [r0]
    mov r0, 2
    jl 0x1f44
    mov r0, 0x90003c
    ld.di r1, [r0]
    or r1, r1, 2
    st.di r1, [r0]
    mov r0, 0x940008
    mov r1, 0x20105
    st.di r1, [r0]
    mov r0, 0x800010
    ld.di r1, [r0]
    or r1, r1, 0x1000000
    st.di r1, [r0]
    mov r0, 0x810060
    mov_s r1, 0
    st.di r1, [r0]
    mov r0, 0x900000
    ld.di r1, [r0]
    or r1, r1, 0x100
    st.di r1, [r0]
    mov r0, 0x800010
    ld.di r1, [r0]
    or r1, r1, 0x41000
    st.di r1, [r0]
    mov r0, 0x8407fc
    ld.di r1, [r0]
    mov r1, 0x7ffffeb9
    st.di r1, [r0]
    mov r0, adc_path_setup
adc_path_next:
    ld.ab r1, [r0, 4]
    breq r1, 0, adc_path_ready
    ld.ab r2, [r0, 4]
    st.di r2, [r1]
    b adc_path_next
adc_path_ready:
    mov r0, 0x850004
    mov_s r1, 1
    st.di r1, [r0]
    mov r0, 0x852004
    ld.di r4, [r0]
    or r1, r4, 0x410000
    st.di r1, [r0]
    mov r0, 100
    jl 0x1f44
    mov r0, 0x85001c
    mov_s r1, 0
    st.di r1, [r0]
    mov r0, 0x850018
    mov_s r1, 0x80
    st.di r1, [r0]
    mov r3, adc_trace
    ld.di r1, [r0]
    st.di r1, [r3, 0]
    mov r2, 300000
capture_adc_wait:
    ld.di r1, [r0]
    breq r1, 0, capture_adc_done
    sub.f r2, r2, 1
    bne capture_adc_wait
    mov_s r1, 1
    st.di r1, [r3, 4]
    mov_s r1, 0
    st.di r1, [r0]
    b capture_adc_record
capture_adc_done:
    st.di r1, [r3, 4]
capture_adc_record:
    mov r0, 0x85001c
    ld.di r1, [r0]
    st.di r1, [r3, 8]
    st.di r2, [r3, 12]
    mov r0, 0x850004
    mov_s r1, 0
    st.di r1, [r0]
    mov r0, 0x852004
    mov r1, 0x001e50a0
    st.di r1, [r0]
    mov r0, 0xf68500
    mov r1, 0xf6f800
    mov r2, 0xa5a5a5a5
capture_clear:
    st.di.ab r2, [r0, 4]
    brlo r0, r1, capture_clear
    mov r0, 0x840280
    mov_s r1, 0
    st.di r1, [r0]
    mov r1, 0x8403b0
    mov_s r2, 0x11
    st.di r2, [r1]
    mov_s r1, 1
    st.di r1, [r0]
    mov r3, adc_trace
    ld.di r2, [r0]
    st.di r2, [r3, 16]
    mov r0, 0x8403b0
    ld.di r2, [r0]
    st.di r2, [r3, 20]
    mov r0, 0x856000
    mov r1, 0xf68500
    st.di r1, [r0, 0x14]
    mov r1, 0xf6f800
    st.di r1, [r0, 0x18]
    mov r1, 0x412
    st.di r1, [r0]
    mov r1, 0x2041a
    st.di r1, [r0]
    b status

capture_stop:
    mov r0, 0x856000
    ld.di r1, [r0]
    bic r1, r1, 0x20008
    st.di r1, [r0]
    mov r0, 0x840280
    mov_s r1, 0
    st.di r1, [r0]
    mov r0, 0xf68500
    mov r1, SAMPLE_PAYLOAD
    mov r2, 3680
    mov r3, 0x00ff00ff
capture_pack:
    ld.di.ab r4, [r0, 4]
    ld.di.ab r5, [r0, 4]
    lsr r4, r4, 4
    asl r5, r5, 4
    and r4, r4, r3
    bic r5, r5, r3
    or r4, r4, r5
    st.di.ab r4, [r1, 4]
    sub.f r2, r2, 1
    bne capture_pack
    b status

pll_lock_sequence:
    push_s blink
    push_s r14
    mov r14, r6
    mov r0, 0x85200c
    ld.di r1, [r0]
    or r1, r1, 0x1000
    st.di r1, [r0]
    mov r0, 2000
    jl 0x1f44
    mov r0, 0x85200c
    ld.di r1, [r0]
    bic r1, r1, 0x1000
    st.di r1, [r0]
    mov r0, 2000
    jl 0x1f44
    mov r0, 0x85200c
    ld.di r1, [r0]
    or r1, r1, 0x1000
    st.di r1, [r0]
    mov r0, pll_uhf_masks
pll_mask_next:
    ld.ab r1, [r0, 4]
    breq r1, 0, pll_latch
    ld.ab r2, [r0, 4]
    ld.ab r3, [r0, 4]
    ld.di r4, [r1]
    bic r4, r4, r2
    or r4, r4, r3
    st.di r4, [r1]
    b pll_mask_next
pll_latch:
    mov r0, 0x852008
    st.di r14, [r0]
    or r1, r14, 2
    st.di r1, [r0]
    mov r0, 10
    jl 0x1f44
    mov r0, 0x852008
    st.di r14, [r0]
    mov r0, 6000
    jl 0x1f44
    mov r0, 0x852014
    ld.di r1, [r0]
    btst r1, 30
    bne pll_sequence_done
    mov r0, 0x85200c
    ld.di r1, [r0]
    or r1, r1, 0x2000
    st.di r1, [r0]
    mov r0, 30000
    jl 0x1f44
    mov r0, 0x85200c
    ld.di r1, [r0]
    bic r1, r1, 0x2000
    st.di r1, [r0]
    mov r0, 2000
    jl 0x1f44
    mov r0, 0x852008
    or r1, r14, 2
    st.di r1, [r0]
    mov r0, 10
    jl 0x1f44
    mov r0, 0x852008
    st.di r14, [r0]
    mov r0, 6000
    jl 0x1f44
pll_sequence_done:
    pop_s r14
    pop_s blink
    j_s [blink]

    .balign 4
pll_uhf_masks:
    .long 0x852040, 0x40004, 0
    .long 0x852040, 0x80, 0x0
    .long 0x852040, 0x100, 0x0
    .long 0x852000, 0x80000, 0x80000
    .long 0x85202c, 0xff00000, 0x0
    .long 0x852034, 0xff0000, 0x0
    .long 0x852040, 0x7e00, 0x2e00
    .long 0x852040, 0x7000000, 0x0
    .long 0x852040, 0x40, 0x40
    .long 0x852040, 0x2, 0x0
    .long 0x852040, 0x20000, 0x0
    .long 0x852040, 0x10000, 0x10000
    .long 0x852040, 0x8, 0x8
    .long 0x852040, 0x20, 0x0
    .long 0x852040, 0x10, 0x10
    .long 0x852040, 0x8000, 0x0
    .long 0x852034, 0x2, 0x0
    .long 0x852034, 0x1, 0x0
    .long 0x852040, 0x1, 0x0
    .long 0x852004, 0x80, 0x80
    .long 0x852040, 0x400000, 0x0
    .long 0x852040, 0x40000000, 0x40000000
    .long 0x852040, 0x80000, 0x0
    .long 0x852008, 0x300, 0x0
    .long 0
    .balign 4
adc_path_setup:
    .long 0x8407f4, 3
    .long 0x841004, 0, 0x841008, 5, 0x84100c, 0
    .long 0x850000, 777, 0x85000c, 0, 0x841018, 1
    .long 0x840038, 0x40, 0x8400e0, 0xb
    .long 0x840258, 4, 0x840258, 0
    .long 0x840044, 0x200, 0x840158, 1, 0x840154, 0
    .long 0x840054, 0, 0x840058, 0
    .long 0x840060, 2, 0x840064, 3, 0x840290, 0xcb
    .long 0x84025c, 0xc00, 0x84030c, 0x8508, 0x840310, 0
    .long 0

rf_setup:
    .long 0x852000, 0x001d897a, 0x852004, 0x001e50a0
    .long 0x85200c, 0x511c54b0, 0x852020, 0x40030155
    .long 0x852024, 0x60606066, 0x85202c, 0x200003c0
    .long 0x852030, 0x5a000000, 0x852034, 0x00000018
    .long 0x85201c, 0x02400200
    .long 0x852018, 0x03f00000, 0x820010, 0x00000010
    .long 0

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
    .long route_trace + 0
    .long route_trace + 4
    .long route_trace + 8
    .long route_trace + 12
    .long route_trace + 16
    .long route_trace + 20
    .long route_trace + 24
    .long route_trace + 28
    .long route_trace + 32
    .long route_trace + 36
    .long route_trace + 40
    .long route_trace + 44
    .long route_trace + 48
    .long route_trace + 52
    .long route_trace + 56
    .long route_trace + 60
    .long route_trace + 64
    .long route_trace + 68
    .long route_trace + 72
    .long route_trace + 76
    .long route_trace + 80
    .long route_trace + 84
    .long route_trace + 88
    .long route_trace + 92
    .long 0x852014, 0x856024
rf_registers_end:
    .long 0

rf_snapshot:
    .short 893
    .byte 11, 0
    .short rf_snapshot_end - rf_snapshot, 0
    .space (rf_registers_end - rf_registers) * 2
rf_snapshot_end:
route_trace:
    .space 96
adc_trace:
    .long 0, 0, 0, 0, 0, 0

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
