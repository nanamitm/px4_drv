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
    .equ ROM_CREATE_TASK, 0x14bdc
    .equ ROM_DELAY, 0x1f44
    /* STATUS32 の割り込み許可 (E1, E2) を落として元の値を返す / 戻す */
    .equ ROM_IRQ_DISABLE, 0xb64
    .equ ROM_IRQ_RESTORE, 0xb94
    /* 連続送出の送信枠。参照 firmware と同じく 0x4000 バイト間隔の 12 枠を順に使う */
    .equ STREAM_SLOTS, 12
    .equ RING_BASE, 0xf68500
    .equ RING_END, 0xf6f800
    /* DMA は 1 ブロック分の環状領域の最後の語を書いて停止する。この語の変化で終端を検出する */
    .equ RING_LAST, 0xf6f7fc
    .equ RING_WORDS, 7360

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
    cmp r1, 668
    beq version
    cmp r1, 890
    beq fixed_data
    mov_s r0, 0
    j_s [blink]

/* BonDriver_S1UD.dll の J.83B 用 firmware と同じ要求 0x37A (= 890、この probe の命令と同じ番号)。
   90 MHz 以上の引数は周波数 (Hz) として連続送出を始め、送出タスクの作成後の 0 は送出を止める。
   応答 0x37B (= 891) は結果と送出の累積統計 (filled, sent, late, dropped, timeouts, errors) を返す。
   PLL とマスクの値を参照 firmware で確認した 138.75 MHz だけを受け付け、ほかは結果 1 を返す。
   入口では fixed_data が blink と r13 を退避し、r13 が要求を指す */
iq_request:
    mov r2, 1
    ld r1, [r13, 8]
    breq r1, 0, iq_accept
    mov r3, 138750000
    brne r1, r3, iq_reply
iq_accept:
    mov r0, stream_state
    ld r3, [r0, 0]
    brne r3, 0, iq_task_ready
    push_s r1
    jl create_stream_task
    pop_s r1
    mov r2, 1
    breq r0, 0, iq_reply
iq_task_ready:
    /* 送出タスクは要求番号の変化で周波数を読み直すため、周波数を先に書く */
    mov r0, stream_req
    st.di r1, [r0, 4]
    ld.di r3, [r0, 0]
    add r3, r3, 1
    st.di r3, [r0, 0]
    mov r2, 0
iq_reply:
    mov r0, iq_result
    st r2, [r0, 8]
    mov r1, stream_stats
    ld.di r3, [r1, 0]
    st r3, [r0, 12]
    ld.di r3, [r1, 4]
    st r3, [r0, 16]
    ld.di r3, [r1, 8]
    st r3, [r0, 20]
    ld.di r3, [r1, 12]
    st r3, [r0, 24]
    ld.di r3, [r1, 16]
    st r3, [r0, 28]
    ld.di r3, [r1, 20]
    st r3, [r0, 32]
    mov r0, r13
    pop_s r13
    pop_s blink
    mov r1, iq_result
    mov_s r2, 36
    b send_reply

/* 参照 firmware の主タスクと同じ引数で ROM に送出タスクを作らせる。成功時は r0 が 0 以外 */
create_stream_task:
    push_s blink
    mov r0, task_name
    mov r1, 0xe
    mov r2, 0
    mov r3, 0x1000
    mov r4, 1
    mov r5, stream_task
    mov r6, 0
    mov r7, 0x1e
    jl ROM_CREATE_TASK
    mov r1, stream_state
    st.di r0, [r1, 4]
    breq r0, 0, create_done
    mov r2, 1
    st.di r2, [r1, 0]
create_done:
    pop_s blink
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
    mov r2, 90000000
    cmp r1, r2
    bhs iq_request
    cmp r1, 0
    bne fixed_command
    mov r2, stream_state
    ld r2, [r2, 0]
    cmp r2, 0
    bne iq_request
    b status
fixed_command:
    cmp r1, 2
    beq snapshot
    cmp r1, 3
    beq pll_readback
    cmp r1, 4
    beq pll_control_readback
    cmp r1, 8
    beq capture_start
    cmp r1, 9
    beq capture_start
    cmp r1, 10
    beq capture_stop
    cmp r1, 11
    beq capture_start
    cmp r1, 12
    beq capture_batch
    cmp r1, 100
    beq poke
    cmp r1, 101
    beq create_task_command
    cmp r1, 24
    bhs gain_test
    cmp r1, 16
    blo select_default_payload
    cmp r1, 23
    bhi status
    sub r1, r1, 16
    asl r1, r1, 14
    add r1, r1, SAMPLE_PAYLOAD
    b select_payload
select_default_payload:
    mov r1, SAMPLE_PAYLOAD
select_payload:
    mov r2, r1

    mov r0, stats
    ld r1, [r0, 20]
    brne r1, 0, status
    mov r3, SAMPLE_DESCRIPTOR
    st.di r2, [r3]
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
    cmp r1, 11
    mov.eq r6, 0xc0017600
    jl capture_tune
    b capture_route

/* r6 の PLL 語で選局し、ADC の経路を設定して環状領域を初期化する。送出タスクと共用する */
capture_tune:
    push_s blink
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
    mov r0, 0x852008
    ld.di r1, [r0]
    cmp r1, 0xc0017600
    bne prepare_capture_ring
    mov r0, 0x852000
    mov r1, 0x00108d59
    st.di r1, [r0]
    mov r1, 0x001e5020
    st.di r1, [r0, 4]
    mov r1, 0x023c2216
    st.di r1, [r0, 0x1c]
    /* PLL のマスク手順でビット 18 を落とすと、138.75 MHz の I/Q が参照 firmware より約 30 dB 小さくなる。
       参照 firmware は送出中にこのビットを立てており (0x852040 = 0xfab67fa1)、立てると RMS が参照と一致した */
    ld.di r1, [r0, 0x40]
    bset r1, r1, 18
    st.di r1, [r0, 0x40]
prepare_capture_ring:
    mov r0, 0xf68500
    mov r1, 0xf6f800
    mov r2, 0xa5a5a5a5
capture_clear:
    st.di.ab r2, [r0, 4]
    brlo r0, r1, capture_clear
    pop_s blink
    j_s [blink]

capture_route:
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
    mov r0, 0x856024
    mov r1, 0x880040
    ld.di r2, [r1]
    ld.di r3, [r0]
    mov r4, r3
    mov r5, 0
    mov r6, 0
    mov r7, 0
    mov r8, 500000
capture_observe:
    ld.di r9, [r0]
    cmp r9, r3
    add.ne r5, r5, 1
    add.lo r6, r6, 1
    cmp r9, r4
    mov.hi r4, r9
    mov r3, r9
    ld.di r9, [r1]
    sub r7, r2, r9
    cmp r7, 10000
    bhs capture_observed
    sub.f r8, r8, 1
    bne capture_observe
capture_observed:
    mov r0, observe_trace
    st.di r3, [r0, 0]
    st.di r4, [r0, 4]
    st.di r5, [r0, 8]
    st.di r6, [r0, 12]
    st.di r7, [r0, 16]
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

/* 調査用: 引数 100 に続く 2 語 (アドレス, 値) を書き、894 で (アドレス, 値, 読み返し) を返す。
   読み出しだけなら ROM の要求 662 (4 バイト範囲の和) で足りる */
poke:
    ldh r1, [r13, 4]
    cmp r1, 20
    blo status
    ld r2, [r13, 12]
    ld r3, [r13, 16]
    st.di r3, [r2]
    ld.di r4, [r2]
    mov r0, pll_result
    st r2, [r0, 8]
    st r3, [r0, 12]
    st r4, [r0, 16]
    mov r0, r13
    pop_s r13
    pop_s blink
    mov r1, pll_result
    mov_s r2, 32
    b send_reply

create_task_command:
    mov r0, stream_state
    ld r1, [r0, 0]
    cmp r1, 0
    bne status
    jl create_stream_task
    b status

gain_test:
    cmp r1, 87
    bhi status
    sub r1, r1, 24
    mov r0, 0x852000
    ld.di r2, [r0]
    bic r2, r2, 0x3f
    or r2, r2, r1
    st.di r2, [r0]
    b status

/* Bounded experiment: fill eight USB slots while alternating two DMA ranges. */
capture_batch:
    push_s r14
    push_s r15
    mov r14, 0
    mov r15, 0xf68500
    mov r0, 0x880040
    ld.di r1, [r0]
    mov r0, batch_trace
    st.di r1, [r0]
    mov r1, 0
    st.di r1, [r0, 4]
    mov r0, r15
    jl arm_capture
    cmp r0, 0
    beq batch_done
batch_next:
    mov r0, 0x856024
    mov r1, 500000
batch_wait_end:
    ld.di r2, [r0]
    cmp r2, r15
    beq batch_full
    sub.f r1, r1, 1
    bne batch_wait_end
    b batch_done
batch_full:
    mov r0, 0x880040
    ld.di r3, [r0]
    mov r0, batch_trace
    asl r1, r14, 2
    add r1, r1, r0
    st.di r3, [r1, 8]
    cmp r14, 7
    beq batch_pack
    mov r0, 0xe40000
    cmp r15, 0xe40000
    mov.eq r0, 0xf68500
    jl arm_capture
    cmp r0, 0
    beq batch_done
batch_pack:
    mov r0, r15
    asl r1, r14, 14
    add r1, r1, SAMPLE_PAYLOAD
    mov r2, 3680
    mov r3, 0x00ff00ff
batch_pack_next:
    ld.di.ab r4, [r0, 4]
    ld.di.ab r5, [r0, 4]
    lsr r4, r4, 4
    asl r5, r5, 4
    and r4, r4, r3
    bic r5, r5, r3
    or r4, r4, r5
    st.di.ab r4, [r1, 4]
    sub.f r2, r2, 1
    bne batch_pack_next
    st.di r14, [r1]
    add r14, r14, 1
    mov r0, batch_trace
    st.di r14, [r0, 4]
    cmp r14, 8
    beq batch_done
    cmp r15, 0xe40000
    beq batch_use_first
    mov r15, 0xe40000
    b batch_next
batch_use_first:
    mov r15, 0xf68500
    b batch_next
batch_done:
    mov r0, 0x856024
    ld.di r1, [r0]
    mov r0, batch_trace
    st.di r1, [r0, 40]
    st.di r15, [r0, 44]
    mov r0, 0x856000
    ld.di r1, [r0]
    bic r1, r1, 0x20008
    st.di r1, [r0]
    mov r0, 0x840280
    mov r1, 0
    st.di r1, [r0]
    pop_s r15
    pop_s r14
    b status

arm_capture:
    mov r4, r0
    mov r1, 0x856000
    add r2, r0, 0x7300
    ld.di r3, [r1, 0x14]
    cmp r0, r3
    blo arm_base_first
    st.di r2, [r1, 0x18]
    st.di r0, [r1, 0x14]
    b arm_range_ready
arm_base_first:
    st.di r0, [r1, 0x14]
    st.di r2, [r1, 0x18]
arm_range_ready:
    ld.di r0, [r1]
    bic r0, r0, 0x400
    st.di r0, [r1]
    or r0, r0, 0x400
    st.di r0, [r1]
    mov r3, 500000
arm_wait_started:
    ld.di r0, [r1, 0x24]
    cmp r0, r4
    bls arm_wait_again
    cmp r0, r2
    blo arm_started
arm_wait_again:
    sub.f r3, r3, 1
    bne arm_wait_started
    mov r0, 0
    j_s [blink]
arm_started:
    mov r0, 1
    j_s [blink]

/* 送出タスク。要求番号 (stream_req+0) が処理済みの番号 (+8) と異なれば周波数 (+4) を読み、
   0 以外なら選局して連続送出する。メッセージ処理とは別の実行文脈で動くため、送出中も停止要求を受け取れる */
stream_task:
stream_idle:
    mov r13, stream_req
    ld.di r0, [r13, 0]
    ld.di r1, [r13, 8]
    breq r0, r1, stream_idle_wait
    st.di r0, [r13, 8]
    ld.di r2, [r13, 4]
    breq r2, 0, stream_idle
    mov r6, 0xc0017600
    jl capture_tune
    jl stream_run
    b stream_idle
stream_idle_wait:
    mov r0, 100
    jl ROM_DELAY
    b stream_idle

/* 参照 firmware の送出処理 (0x48a20) と同じ方式。環状領域は 1 ブロック分で、DMA の書き込み位置を追って
   8 ビットへ詰め、DMA が最後の語を書いたら直ちに再始動する。再始動が遅れた (待つ前に最後の語が
   書かれていた) ブロックの次は、番号の上位 2 ビットへ 2 を入れる。通常は 1 */
stream_run:
    push_s blink
    jl stream_init_slots
    mov r14, 0
    mov r17, 0
    mov r18, 2
    jl ROM_IRQ_DISABLE
    mov r23, r0
    mov r0, RING_LAST
    mov_s r1, 0
    st.di r1, [r0]
    mov r0, 0x840280
    st.di r1, [r0]
    mov r2, 0x856000
    ld.di r3, [r2]
    and r3, r3, 0xfffdfff7
    st.di r3, [r2]
    mov r3, 0x8403b0
    mov r1, 0x11
    st.di r1, [r3]
    mov_s r1, 1
    st.di r1, [r0]
    jl stream_arm
    mov r2, 0x856000
    ld.di r3, [r2]
    or r3, r3, 0x20008
    st.di r3, [r2]
    mov r0, r23
    jl ROM_IRQ_RESTORE
stream_block:
    mov r13, stream_req
    ld.di r0, [r13, 0]
    ld.di r1, [r13, 8]
    cmp r0, r1
    bne stream_stop
    asl r16, r14, 14
    add r16, r16, SAMPLE_PAYLOAD
    mov r0, slot_busy
    add r0, r0, r14
    ldb.di r19, [r0]
    mov r15, 0
    mov r21, 1
    mov r22, 2000000
stream_chase:
    sub.f r22, r22, 1
    beq stream_timeout
    mov r0, 0x856024
    ld.di r0, [r0]
    add r2, r0, 0x1800
    mov r3, RING_LAST + 3
    brlo r3, r2, stream_end
    mov r3, RING_LAST
    ld.di r2, [r3]
    brne r2, 0, stream_end
    mov r3, RING_BASE
    brlo r0, r3, stream_chase
    sub r0, r0, r3
    lsr r0, r0, 2
    bclr r0, r0, 0
    add r2, r15, 256
    brlo r0, r2, stream_chase
    mov r20, r0
    brne r19, 0, stream_chase_skip
    mov r1, r20
    jl stream_pack
stream_chase_skip:
    mov r15, r20
    b stream_chase

stream_end:
    /* 終端の待機から再始動までは割り込みを止める (参照 firmware も 0xb64 と 0xb94 で囲む)。
       割り込みで再始動が遅れると、ブロック間の欠落が復調器の想定する 4 から 7 サンプルを超える */
    jl ROM_IRQ_DISABLE
    mov r23, r0
    mov r3, RING_LAST
    ld.di r20, [r3]
    mov r2, 200000
stream_end_wait:
    ld.di r0, [r3]
    brne r0, 0, stream_rearm
    sub.f r2, r2, 1
    bne stream_end_wait
    mov r21, 2
    mov r0, stream_stats
    ld.di r1, [r0, 16]
    add r1, r1, 1
    st.di r1, [r0, 16]
stream_rearm:
    jl stream_arm
    mov r2, 200000
stream_restart_wait:
    mov r0, 0x856024
    ld.di r0, [r0]
    sub r0, r0, RING_BASE + 1
    cmp r0, 0x72ff
    blo stream_restarted
    sub.f r2, r2, 1
    bne stream_restart_wait
stream_restarted:
    mov r0, r23
    jl ROM_IRQ_RESTORE
    breq r20, 0, stream_tail
    mov r21, 2
    mov r0, stream_stats
    ld.di r1, [r0, 8]
    add r1, r1, 1
    st.di r1, [r0, 8]
stream_tail:
    brne r19, 0, stream_tail_done
    mov r1, RING_WORDS
    jl stream_pack
stream_tail_done:
    mov r0, RING_LAST
    mov_s r1, 0
    st.di r1, [r0]
    jl stream_submit
    mov r18, r21
    add r17, r17, 1
    add r14, r14, 1
    cmp r14, STREAM_SLOTS
    mov.hs r14, 0
    b stream_block

/* DMA の書き込み位置が進まない。捕捉を止めて最初から始め直す */
stream_timeout:
    mov r0, stream_stats
    ld.di r1, [r0, 16]
    add r1, r1, 1
    st.di r1, [r0, 16]
    jl stream_disable
    mov r0, RING_LAST
    mov_s r1, 0
    st.di r1, [r0]
    mov r18, 2
    mov r0, 0x840280
    mov_s r1, 1
    st.di r1, [r0]
    jl stream_arm
    mov r2, 0x856000
    ld.di r3, [r2]
    or r3, r3, 0x20008
    st.di r3, [r2]
    b stream_block

stream_stop:
    jl stream_disable
    /* USB の完了を最大 100 ms 待ち、送信中の枠を解放してから待機へ戻る */
    mov r15, 1000
stream_stop_wait:
    mov r0, slot_busy
    mov r1, 0
    mov r2, STREAM_SLOTS
stream_stop_scan:
    ldb.di r3, [r0]
    or r1, r1, r3
    add r0, r0, 1
    sub.f r2, r2, 1
    bne stream_stop_scan
    breq r1, 0, stream_stopped
    mov r0, 100
    jl ROM_DELAY
    sub.f r15, r15, 1
    bne stream_stop_wait
stream_stopped:
    pop_s blink
    j_s [blink]

stream_disable:
    mov r2, 0x856000
    ld.di r3, [r2]
    and r3, r3, 0xfffdfff7
    st.di r3, [r2]
    mov r0, 0x840280
    mov_s r1, 0
    st.di r1, [r0]
    j_s [blink]

/* 参照 firmware の 0x488a8 と同じ再始動。範囲を設定して 0x856000 のビット 10 を立てる */
stream_arm:
    mov r0, 0x856000
    mov r1, RING_BASE
    st.di r1, [r0, 0x14]
    mov r1, RING_END
    st.di r1, [r0, 0x18]
    ld.di r1, [r0]
    bset r1, r1, 10
    st.di r1, [r0]
    j_s [blink]

/* 環状領域の語 r15 から r1 の手前までを r16 の枠へ詰める。1 語は 16 ビットの I と Q で、
   2 語から I0, I1, Q0, Q1 の 4 バイトを作る。参照 firmware の 0x48938 と同じビット位置 */
stream_pack:
    sub r2, r1, r15
    lsr.f r2, r2, 1
    jeq_s [blink]
    asl r0, r15, 2
    add r0, r0, RING_BASE
    asl r4, r15, 1
    add r4, r4, r16
    mov r3, 0x00ff00ff
    mov lp_count, r2
    nop_s
    lp stream_pack_end
    ld.ab r5, [r0, 4]
    ld.ab r6, [r0, 4]
    lsr r5, r5, 4
    asl r6, r6, 4
    and r5, r5, r3
    bic r6, r6, r3
    or r5, r5, r6
    st.ab r5, [r4, 4]
stream_pack_end:
    j_s [blink]

/* 番号を付けて USB へ渡す。枠が送信中なら詰めずに捨て、dropped を数える */
stream_submit:
    push_s blink
    mov r0, stream_stats
    ld.di r1, [r0, 0]
    add r1, r1, 1
    st.di r1, [r0, 0]
    brne r19, 0, stream_submit_drop
    and r0, r17, 0xfffff
    asl r1, r18, 20
    or r0, r0, r1
    add r1, r16, 14720
    st.di r0, [r1]
    mov r0, slot_busy
    add r20, r0, r14
    mov_s r1, 1
    stb.di r1, [r20]
    /* ARC700 の構成に乗算器がない前提で、枠番号 * 12 をシフトで求める */
    asl r2, r14, 3
    asl r3, r14, 2
    add r2, r2, r3
    add r2, r2, SAMPLE_HEADER
    mov_s r0, 0
    mov_s r1, 1
    mov_s r3, 0
    jl ROM_SEND_BULK
    mov r1, stream_stats
    breq r0, 0, stream_submit_reject
    ld.di r2, [r1, 4]
    add r2, r2, 1
    st.di r2, [r1, 4]
    pop_s blink
    j_s [blink]
stream_submit_reject:
    mov_s r0, 0
    stb.di r0, [r20]
    ld.di r2, [r1, 20]
    add r2, r2, 1
    st.di r2, [r1, 20]
stream_submit_drop:
    mov r1, stream_stats
    ld.di r2, [r1, 12]
    add r2, r2, 1
    st.di r2, [r1, 12]
    pop_s blink
    j_s [blink]

/* 12 枠の HIF ヘッダーと DMA 記述子。参照 firmware の 0x48984 と同じ配置 */
stream_init_slots:
    mov r0, SAMPLE_HEADER
    mov r1, SAMPLE_DESCRIPTOR
    mov r2, SAMPLE_PAYLOAD
    mov r3, STREAM_SLOTS
    mov r5, slot_busy
stream_init_next:
    mov r4, 0x0100037c
    st.di r4, [r0, 0]
    mov r4, 0x000d398c
    st.di r4, [r0, 4]
    st.di r1, [r0, 8]
    st.di r2, [r1, 0]
    mov r4, 0
    st.di r4, [r1, 4]
    st.di r4, [r1, 8]
    mov r4, 14724
    st.di r4, [r1, 12]
    mov r4, 0
    stb.di r4, [r5]
    add r0, r0, 12
    add r1, r1, 16
    add r2, r2, 0x4000
    add r5, r5, 1
    sub.f r3, r3, 1
    bne stream_init_next
    j_s [blink]

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
    cmp r14, 0xc0017600
    mov.eq r0, pll_cable_masks
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
pll_cable_masks:
    .long 0x852040, 0x40004, 0
    .long 0x852040, 0x80, 0x80
    .long 0x852040, 0x100, 0x100
    .long 0x852000, 0x80000, 0x0
    .long 0x85202c, 0xff00000, 0x0
    .long 0x852034, 0xff0000, 0x0
    .long 0x852040, 0x7e00, 0x7e00
    .long 0x852040, 0x7000000, 0x2000000
    .long 0x852040, 0x40, 0x0
    .long 0x852040, 0x2, 0x0
    .long 0x852040, 0x20000, 0x20000
    .long 0x852040, 0x10000, 0x0
    .long 0x852040, 0x8, 0x0
    .long 0x852040, 0x20, 0x20
    .long 0x852040, 0x10, 0x0
    .long 0x852040, 0x8000, 0x0
    .long 0x852034, 0x2, 0x0
    .long 0x852034, 0x1, 0x0
    .long 0x852040, 0x1, 0x1
    .long 0x852004, 0x80, 0x0
    .long 0x852040, 0x400000, 0x0
    .long 0x852040, 0x40000000, 0x40000000
    .long 0x852040, 0x80000, 0x0
    .long 0x852008, 0x300, 0x200
    .long 0

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
    /* 送出の枠のヘッダーなら、その枠を空きに戻す。枠 0 は従来の単発送信と共用する */
    mov r2, SAMPLE_HEADER
    sub r3, r1, r2
    cmp r3, STREAM_SLOTS * 12
    bhs complete_single
    mov r4, slot_busy
complete_find:
    brlo r3, 12, complete_slot
    sub r3, r3, 12
    add r4, r4, 1
    b complete_find
complete_slot:
    brne r3, 0, complete_single
    mov_s r3, 0
    stb.di r3, [r4]
complete_single:
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

iq_result:
    .short 0x37b
    .byte 11, 0
    .short 36, 0
    /* 結果, filled, sent, late, dropped, timeouts, errors */
    .long 0, 0, 0, 0, 0, 0, 0

    .balign 4
/* 要求番号, 周波数 (Hz), 送出タスクが処理した要求番号 */
stream_req:
    .long 0, 0, 0
/* 作成済み, ROM が返したタスク */
stream_state:
    .long 0, 0
/* filled, sent, late, dropped, timeouts, errors */
stream_stats:
    .long 0, 0, 0, 0, 0, 0
slot_busy:
    .space 12
task_name:
    .asciz "s1ud_iq"
    .balign 4

pll_result:
    .short 894
    .byte 11, 0
    .short 32, 0
    .long 0, 0, 0, 0, 0, 0

rf_registers:
    .long batch_trace + 0
    .long batch_trace + 4
    .long batch_trace + 8
    .long batch_trace + 12
    .long batch_trace + 16
    .long batch_trace + 20
    .long batch_trace + 24
    .long batch_trace + 28
    .long batch_trace + 32
    .long batch_trace + 36
    .long batch_trace + 40, batch_trace + 44
    .long 0x852000, 0x852008, 0x852014, 0x856024
rf_registers_end:
    .long 0

rf_snapshot:
    .short 893
    .byte 11, 0
    .short rf_snapshot_end - rf_snapshot, 0
    .space (rf_registers_end - rf_registers) * 2
rf_snapshot_end:
batch_trace:
    .space 48
observe_trace:
    .space 20
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
