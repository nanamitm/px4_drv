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
    /* 送信枠の上限。枠の数と配置は slot_config で変えられ、既定は参照 firmware と同じ 12 枠 */
    .equ STREAM_SLOTS_MAX, 64
    .equ RING_BASE, 0xf68500
    .equ RING_END, 0xf6f800
    /* DMA は 1 ブロック分の環状領域の最後の語を書いて停止する。この語の変化で終端を検出する */
    .equ RING_LAST, 0xf6f7fc
    .equ RING_WORDS, 7360
    /* 別の firmware への読み込み直し。参照 firmware と同じく、0xe00000 からのアドレスで受け取った
       イメージを 0xe20000 へ置く (0xe00000 からの 64 KB は 0xe10000 に鏡像があり、イメージ全体を置けない)。
       本体は先頭 0xa0 バイトを除いて 0x40300 へ写し、ROM のソフトリセットで新しい firmware の warm 入口へ移る */
    .equ STAGING_ADDRESS, 0xe00000
    .equ STAGING_BASE, 0xe20000
    .equ STAGING_SIZE, 0x19da0
    .equ IMAGE_PREFIX, 0xa0
    .equ ROM_WORK, 0x40240
    .equ ROM_WORK_SAVE, 0xe50000
    .equ ROM_WARM_ENTRY, 0x40200
    .equ ROM_SOFT_RESET, 0x123c
    .equ COLD_STACK_TOP, 0xe6f000
    /* warm 起動で ROM の USB の状態を保つための ROM の入口 (参照 firmware が同じ目的で呼ぶもの) */
    .equ ROM_ZERO, 0x488
    .equ ROM_USB_SUSPEND, 0x1604
    .equ ROM_USB_ENDPOINTS, 0x58c0
    .equ ROM_USB_PREPARE, 0x5fa4
    .equ ROM_USB_PIPE, 0x6030
    .equ ROM_USB_SPEED, 0x6d20
    .equ ROM_USB_TIMER, 0x11524
    .equ ROM_USB_ATTACH, 0x128dc
    .equ ROM_USB_NOTIFY, 0x12bbc
    .equ ROM_USB_CONFIGURE, 0x13130
    .equ USB_SAVE, 0xe40000
    .equ USB_SAVE_MAGIC, 0xaa11bb22

    .section .text
    .global _start
/* 先頭 0xa0 バイトは参照 firmware と同じ配置にする。ROM と他の firmware は 0x10 の warm 入口と
   0x14 の 0xe00000 を確かめ、読み込み直しでは 0xa0 以降だけを 0x40300 へ写す */
_start:
    b cold_start
    .org 0x10
    .long warm_entry
    .long STAGING_ADDRESS

/* 0x40300 以降を書き換える間も残る位置 (先頭 0xa0 バイト) で、読み込み直しを実行する。
   r0 は新しい firmware の warm 入口。ROM の作業領域 0x40240 から 32 バイトを 0xe50000 へ退避し
   (新しい firmware の warm 入口が戻す)、0x40200 に入口を置いてソフトリセットする */
reload_exec:
    mov r13, r0
    jl ROM_IRQ_DISABLE
    mov r0, ROM_WORK_SAVE
    mov r1, ROM_WORK
    mov r2, 0x20
    jl ROM_COPY
    mov r0, ROM_WARM_ENTRY
    st.di r13, [r0]
    mov r0, 0x40300
    mov r1, STAGING_BASE + IMAGE_PREFIX
    mov r2, 0x19d00
    jl ROM_COPY
    jl ROM_SOFT_RESET
reload_hang:
    b reload_hang
    .org IMAGE_PREFIX

cold_start:
    push_s blink
    /* ROM の 0x19f0 は 0x1000 バイトの起動スタックで動作を確かめた。DriverHost の cold 起動は 0x200 バイトを
       指定するため、未使用の RAM の末尾 (送出の枠と読み込み先より後ろ) のスタックへ切り替えて呼ぶ */
    mov r1, sp
    mov sp, COLD_STACK_TOP
    push_s r1
    mov r0, board_clock_config
    jl 0x19f0
    pop_s r1
    mov sp, r1
    pop_s blink
/* cold 起動と warm 起動の共通の初期化。ROM の主処理が 0x4021c の開始処理を重ねて呼ばないよう、先に消す */
app_init:
    push_s blink
    mov r0, ROM_WARM_ENTRY + 0x1c
    mov_s r1, 0
    st.di r1, [r0]
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
    cmp r1, 702
    beq reload_start
    cmp r1, 660
    beq staging_write
    cmp r1, 662
    beq staging_check
    cmp r1, 704
    beq reload_request
    cmp r1, 578
    beq init_device
    mov_s r0, 0
    j_s [blink]

/* 578 (INIT_DEVICE)。DriverHost は読み込み直した firmware が ISDB-T のモードでないときに送る。
   この firmware は設定を持たないため、579 で 0 を返す */
init_device:
    push_s blink
    push_s r13
    mov r13, r0
    mov r2, 0
    mov r3, 579
    b small_reply

/* DriverHost の Reload と同じ手順 (702, 660, 662, 704) で別の firmware へ読み込み直す。
   702 は送出を止めて 703 を返す。送出の枠と読み込み先 (0xe20000 以降) が重なるため、先に止める */
reload_start:
    push_s blink
    push_s r13
    mov r13, r0
    mov r0, stream_state
    ld r1, [r0, 0]
    breq r1, 0, reload_start_reply
    mov r0, stream_req
    mov r1, 0
    st.di r1, [r0, 4]
    ld.di r1, [r0, 0]
    add r1, r1, 1
    st.di r1, [r0, 0]
reload_start_reply:
    mov r2, 0
    mov r3, 703
    b small_reply

/* 660: 0xe00000 からのアドレスのデータを 0xe20000 以降へ書く。範囲外は 661 で 1 を返す */
staging_write:
    push_s blink
    push_s r13
    mov r13, r0
    mov r2, 1
    ldh r4, [r13, 4]
    cmp r4, 12
    bls staging_write_reply
    sub r4, r4, 12
    ld r0, [r13, 8]
    jl staging_map
    mov r2, 1
    breq r0, 0, staging_write_reply
    add r1, r13, 12
    mov r2, r4
    jl ROM_COPY
    mov r2, 0
staging_write_reply:
    mov r3, 661
    b small_reply

/* 662: 読み込み先の範囲なら、その語の和を 663 で返す。それ以外は ROM に任せる (レジスターの読み出しに使う) */
staging_check:
    ld r2, [r0, 8]
    mov r3, STAGING_ADDRESS
    cmp r2, r3
    blo staging_check_rom
    mov r3, STAGING_ADDRESS + 0x20000
    cmp r2, r3
    bhs staging_check_rom
    push_s blink
    push_s r13
    mov r13, r0
    ld r4, [r13, 12]
    mov r0, r2
    jl staging_map
    mov r2, 0
    breq r0, 0, staging_check_reply
    lsr r4, r4, 2
staging_check_sum:
    breq r4, 0, staging_check_reply
    ld.ab r5, [r0, 4]
    add r2, r2, r5
    sub r4, r4, 1
    b staging_check_sum
staging_check_reply:
    mov r3, 663
    b small_reply
staging_check_rom:
    mov_s r0, 0
    j_s [blink]

/* r0 (0xe00000 からのアドレス) と r4 (バイト数) を読み込み先のアドレスへ変える。範囲外なら 0 */
staging_map:
    mov r1, STAGING_ADDRESS
    sub r0, r0, r1
    mov r3, STAGING_SIZE
    cmp r0, r3
    bhs staging_map_bad
    add r2, r0, r4
    cmp r2, r3
    bhi staging_map_bad
    mov r1, STAGING_BASE
    add r0, r0, r1
    j_s [blink]
staging_map_bad:
    mov r0, 0
    j_s [blink]

/* 704: ROM と同じく、置いたイメージの warm 入口 (0x10) が 0x4025d から 0x59fff、0x14 が 0xe00000 かを
   確かめてから読み込み直す。条件を満たさなければ 705 で 1 を返す */
reload_request:
    push_s blink
    push_s r13
    mov r13, r0
    mov r0, STAGING_BASE
    ld r1, [r0, 0x14]
    mov r2, STAGING_ADDRESS
    brne r1, r2, reload_reject
    ld r1, [r0, 0x10]
    mov r2, 0x4025d
    cmp r1, r2
    blo reload_reject
    mov r2, 0x5a000
    cmp r1, r2
    bhs reload_reject
    mov r0, r1
    j reload_exec
reload_reject:
    mov r2, 1
    mov r3, 705
    b small_reply

/* r3 の種類と r2 の 1 語で応答する。入口で blink と r13 を退避し、r13 が要求を指す */
small_reply:
    mov r0, small_frame
    sth r3, [r0, 0]
    st r2, [r0, 8]
    mov r0, r13
    pop_s r13
    pop_s blink
    mov r1, small_frame
    mov_s r2, 12
    b send_reply

/* warm 入口 (ROM のリセット処理が r0 にリセット要因を入れて呼ぶ)。別の firmware からこの firmware へ読み込み直すと、
   CPU だけがリセットされ、USB コントローラーとホストの接続は残る。ROM の通常の初期化に任せると、ホストは改めて
   列挙せず、要求がすべて時間切れになった (README の実験)。そこで ROM が初期化する前に ROM の USB の状態を
   リセットの影響を受けない 0xe40000 へ写し、0x40218 の復元処理で ROM の初期化の後に戻す */
warm_entry:
    push_s blink
    st.aw r18, [sp, -4]
    st.aw r17, [sp, -4]
    st.aw r16, [sp, -4]
    push_s r15
    push_s r14
    push_s r13
    /* 読み込み直す前の firmware が 0xe50000 へ退避した ROM の作業領域を戻す。0x40250 はリセット要因で、
       ROM が今回書いた値があればそれを残し、なければ引数の値を置く */
    mov r2, ROM_WORK + 0x10
    ld.di r2, [r2]
    mov r3, ROM_WORK_SAVE
    mov r4, ROM_WORK
    mov r5, 8
warm_work_restore:
    ld.ab r1, [r3, 4]
    st.di r1, [r4]
    add r4, r4, 4
    sub.f r5, r5, 1
    bne warm_work_restore
    mov r1, ROM_WORK + 0x10
    cmp r2, 0
    mov.ne r0, r2
    st.di r0, [r1]
    /* 0x40200 からのフック (16 語) を消す。最後に必要なものだけ置き直す */
    mov r3, ROM_WARM_ENTRY
    mov r5, 16
    mov_s r1, 0
warm_hooks_clear:
    st.di r1, [r3]
    add r3, r3, 4
    sub.f r5, r5, 1
    bne warm_hooks_clear
    /* クロックを読み込み直しの前と同じ状態へ戻す。cold 起動で使う ROM の 0x19f0 は、USB の処理と並べて
       呼ぶと応答が止まったため、ここでは同じレジスターを順に書く */
    jl ROM_IRQ_DISABLE
    mov r18, r0
    mov r16, 0x820054
    mov r17, 0x820008
    mov r13, 0x820068
    mov r14, 0x90003c
    ld.di r0, [r16]
    bclr r0, r0, 6
    st.di r0, [r16]
    ld.di r0, [r17]
    and r0, r0, 0xff000fff
    st.di r0, [r17]
    ld.di r0, [r13]
    bclr r0, r0, 6
    st.di r0, [r13]
    mov r0, 2
    jl ROM_DELAY
    ld.di r0, [r14]
    bset r0, r0, 0
    st.di r0, [r14]
    mov r0, 2
    jl ROM_DELAY
    mov r0, 0x820000
    ld.di r1, [r0]
    bclr r1, r1, 2
    bclr r1, r1, 18
    st.di r1, [r0]
    ld.di r1, [r13]
    bclr r1, r1, 0
    bclr r1, r1, 1
    st.di r1, [r13]
    mov r0, 0x820000
    ld.di r1, [r0]
    or r1, r1, 0xe02000
    st.di r1, [r0]
    mov r0, 2
    jl ROM_DELAY
    ld.di r0, [r13]
    bset r0, r0, 6
    st.di r0, [r13]
    mov r0, 0x07ffffff
    st.di r0, [r17]
    ld.di r0, [r16]
    bset r0, r0, 6
    st.di r0, [r16]
    mov r0, 2
    jl ROM_DELAY
    ld.di r0, [r14]
    bset r0, r0, 1
    st.di r0, [r14]
    mov r0, r18
    jl ROM_IRQ_RESTORE
    mov r0, 0
    jl ROM_USB_PREPARE
    mov r0, 1
    jl ROM_USB_PREPARE
    /* ROM の USB の状態を 0xe40000 へ写す */
    mov r14, USB_SAVE
    mov r0, USB_SAVE_MAGIC
    st r0, [r14, 0]
    mov r1, 0xf67a38
    ld r0, [r1]
    st r0, [r14, 4]
    mov r1, 0xf67fa8
    ld r0, [r1]
    st r0, [r14, 8]
    mov r1, 0xf67f10
    ldh r0, [r1]
    sth r0, [r14, 0xc]
    ldh r0, [r1, 2]
    sth r0, [r14, 0xe]
    mov r1, 0xf66bc4
    ldb r0, [r1]
    stb r0, [r14, 0x10]
    mov r15, 0xf67a30
    ld r15, [r15]
    mov r16, usb_save_table
warm_save_next:
    ld r0, [r16, 0]
    breq r0, 0, warm_save_variable
    ld r1, [r16, 4]
    ld r2, [r16, 8]
    /* 写し元が 0xf00000 以上なら ROM の変数で、その値 (ヒープ上の領域) から写す。
       小さい値は 0xf67a30 が指す構造の中の位置で、そこの語が指す領域から写す */
    mov r3, 0xf00000
    cmp r1, r3
    blo warm_save_field
    ld r1, [r1]
    b warm_save_copy
warm_save_field:
    add r1, r1, r15
    ld r1, [r1]
warm_save_copy:
    jl ROM_COPY
    add r16, r16, 12
    b warm_save_next
warm_save_variable:
    /* 長さが 0xe40004 にある可変長の領域。1 から 0x400 バイトのときだけ写す */
    ld r2, [r14, 4]
    sub r0, r2, 1
    mov r3, 0x3ff
    cmp r0, r3
    bhi warm_save_done
    mov r1, 0xf67a3c
    ld r1, [r1]
    mov r0, USB_SAVE + 0x2da
    jl ROM_COPY
warm_save_done:
    jl ROM_USB_SUSPEND
    /* 先頭 0xa0 バイトは読み込み直しで写されないため、置いたイメージから戻す */
    mov r0, 0x40260
    mov r1, STAGING_BASE
    mov r2, IMAGE_PREFIX
    jl ROM_COPY
    mov r0, ROM_HIF_HOOK
    mov r1, handle_message
    st.di r1, [r0]
    mov r0, ROM_WARM_ENTRY + 0x18
    mov r1, warm_restore
    st.di r1, [r0]
    mov r0, ROM_WARM_ENTRY + 0x1c
    mov r1, 0x40260
    st.di r1, [r0]
    mov r0, ROM_WARM_ENTRY + 0x3c
    mov r1, idle_hook
    st.di r1, [r0]
    mov_s r0, 1
    pop_s r13
    pop_s r14
    pop_s r15
    ld.ab r16, [sp, 4]
    ld.ab r17, [sp, 4]
    ld.ab r18, [sp, 4]
    pop_s blink
    j_s [blink]

/* 0x4023c のフック。r1 が 0 なら 1 を返す */
idle_hook:
    tst r1, r1
    mov r0, 1
    mov.ne r0, 0
    j_s [blink]

/* ROM のヒープに len (r1) バイトを確保し、src (r0) を写して返す。確保できなければ ROM のリセットに任せる */
usb_alloc_copy:
    push_s blink
    push_s r13
    push_s r14
    push_s r15
    mov r14, r1
    mov r15, r0
    mov r0, r1
    jl ROM_ALLOC
    mov.f r13, r0
    jleq ROM_SOFT_RESET
    mov r0, r13
    mov r2, r14
    mov r1, r15
    jl ROM_COPY
    mov r0, r13
    pop_s r15
    pop_s r14
    pop_s r13
    pop_s blink
    j_s [blink]

/* ROM のヒープに len (r0) バイトを確保し、0 で埋めて返す */
usb_alloc_zero:
    push_s blink
    push_s r13
    push_s r14
    mov r14, r0
    jl ROM_ALLOC
    mov.f r13, r0
    jleq ROM_SOFT_RESET
    mov r0, r13
    mov r1, r14
    jl ROM_ZERO
    mov r0, r13
    pop_s r14
    pop_s r13
    pop_s blink
    j_s [blink]

/* 0x40218 の復元処理。ROM がヒープと USB を初期化した後に呼ぶ。0xe40000 の写しから ROM の USB の変数と
   構造を戻し、ホストとの接続をそのまま使える状態にしてから、この firmware の初期化 (app_init) へ進む */
warm_restore:
    push_s blink
    st.aw r20, [sp, -4]
    st.aw r19, [sp, -4]
    st.aw r18, [sp, -4]
    st.aw r17, [sp, -4]
    st.aw r16, [sp, -4]
    push_s r15
    push_s r14
    push_s r13
    mov r1, 0x800010
    ld.di r0, [r1]
    bclr r0, r0, 8
    st.di r0, [r1]
    mov r0, ROM_WORK + 0x18
    ld.di r0, [r0]
    brne r0, 0, warm_restore_check
    mov r0, 1
    mov r1, 0x61a80
    mov r2, 2
    jl ROM_USB_TIMER
warm_restore_check:
    mov r0, USB_SAVE
    ld r0, [r0]
    mov r1, USB_SAVE_MAGIC
    cmp r0, r1
    bne warm_restore_done
    mov r0, USB_SAVE + 0x14
    mov r1, 0x20
    jl usb_alloc_copy
    mov r14, r0
    mov r0, USB_SAVE + 0x34
    mov r1, 0x50
    jl usb_alloc_copy
    mov r17, r0
    mov r0, USB_SAVE + 0x84
    mov r1, 0x38
    jl usb_alloc_copy
    mov r20, r0
    mov r0, USB_SAVE + 0xbc
    mov r1, 404
    jl usb_alloc_copy
    mov r13, r0
    mov r0, USB_SAVE + 0x250
    mov r1, 0x20
    jl usb_alloc_copy
    mov r18, r0
    mov r0, USB_SAVE + 0x270
    mov r1, 0x40
    jl usb_alloc_copy
    mov r16, r0
    mov r1, 0xf67a34
    st r16, [r1]
    mov r1, 0xf67a24
    st r14, [r1]
    mov r1, 0xf67a28
    st r17, [r1]
    mov r1, 0xf67a2c
    st r20, [r1]
    mov r1, 0xf67a30
    st r13, [r1]
    mov r1, 0xf67a44
    st r18, [r1]
    mov r0, USB_SAVE + 0x2b0
    mov r1, 0x28
    jl usb_alloc_copy
    st r0, [r13, 0x30]
    mov r0, USB_SAVE + 0x2d8
    mov r1, 2
    jl usb_alloc_copy
    st r0, [r13, 0x34]
    mov r1, USB_SAVE
    ld r1, [r1, 4]
    sub r0, r1, 1
    mov r2, 0x3ff
    cmp r0, r2
    bhi warm_restore_scalars
    mov r0, USB_SAVE + 0x2da
    jl usb_alloc_copy
    mov r1, 0xf67a3c
    st r0, [r1]
warm_restore_scalars:
    mov r0, USB_SAVE
    ld r1, [r0, 4]
    mov r2, 0xf67a38
    st r1, [r2]
    ldh r1, [r0, 0xe]
    mov r2, 0xf67f12
    sth r1, [r2]
    ldh r1, [r0, 0xc]
    mov r2, 0xf67f10
    sth r1, [r2]
    ld r1, [r0, 8]
    mov r2, 0xf67fa8
    st r1, [r2]
    ldb r1, [r0, 0x10]
    mov r2, 0xf66bc4
    stb r1, [r2]
    /* ROM の USB の処理表を ROM の関数で埋め直す */
    mov r1, usb_rom_words
warm_restore_words:
    ld r2, [r1, 0]
    breq r2, 0, warm_restore_links
    ld r3, [r1, 4]
    st r3, [r2]
    add r1, r1, 8
    b warm_restore_words
warm_restore_links:
    mov r0, 0xf67fa4
    st r13, [r0]
    mov r0, 0xf67f8c
    st r0, [r17, 76]
    mov r0, 0x20
    jl usb_alloc_zero
    mov r1, 0xf67a60
    st r0, [r1]
    mov r0, 512
    jl usb_alloc_zero
    mov r1, 0xf67a64
    st r0, [r1]
    st r13, [r14, 0xc]
    st r17, [r14, 4]
    mov r0, 0
    stb r0, [r14, 0x14]
    mov r0, 0x18
    jl usb_alloc_zero
    st r0, [r14, 0x18]
    mov r0, 256
    jl usb_alloc_zero
    st r0, [r14, 0x1c]
    st r14, [r13, 0x20]
    st r18, [r13, 28]
    mov r0, 0xf67f70
    mov r1, 0x1c
    jl ROM_ZERO
    mov r0, 0xf67f70
    mov r1, 0x6750
    st r1, [r0]
    mov r1, 0x6768
    st r1, [r0, 4]
    st r0, [r13, 0]
    add r1, r13, 56
    st r1, [r13, 4]
    mov r1, 0x13130
    st r1, [r18, 8]
    mov r1, 0x1319c
    st r1, [r18, 24]
    st r20, [r17, 8]
    ld r1, [r17, 20]
    mov r0, 0xc40e00
    st.di r0, [r1]
    add r0, r13, 152
    st r0, [r13, 152]
    st r0, [r13, 156]
    add r0, r13, 220
    add r2, r13, 8
    add r1, r13, 136
    st r0, [r13, 220]
    st r0, [r13, 224]
    add r0, r13, 204
    st r13, [r13, 0x78]
    st r0, [r13, 0xc]
    st r0, [r13, 136]
    st r13, [r13, 188]
    st r2, [r13, 140]
    st r1, [r13, 8]
    st.as r13, [r13, 64]
    st r2, [r13, 204]
    st r1, [r13, 208]
    mov r0, r17
    jl ROM_USB_SPEED
    add r20, r13, 124
    add r19, r13, 192
    /* 高速 (戻り値 3) と全速で、ROM の記述子の表が異なる */
    mov r15, 0xf66bc0
    cmp r0, 3
    mov.eq r15, 0xf66bbc
    ld r15, [r15]
    add r17, r16, 24
    mov r0, r17
    mov r1, 0x19ff4
    mov r2, 7
    jl ROM_COPY
    ldh r0, [r15, 0]
    st r20, [r16, 8]
    sth r0, [r16, 28]
    st r17, [r13, 148]
    add r17, r16, 0x21
    mov r0, r17
    mov r1, 0x1a000
    mov r2, 7
    jl ROM_COPY
    ldh r0, [r15, 2]
    stb r0, [r16, 37]
    st r19, [r16, 12]
    lsr r0, r0, 8
    stb r0, [r16, 38]
    ld r0, [r13, 4]
    st r17, [r13, 216]
    add r1, r0, 12
    st r1, [r0, 0xc]
    st r1, [r0, 0x10]
    add r1, r13, 84
    st r1, [r13, 0x54]
    st r1, [r13, 0x58]
    mov r1, 0xf67f54
    st r1, [r0, 8]
    mov r0, 0
    stb r0, [r14, 0x14]
    mov r1, ROM_WARM_ENTRY + 0x3c
    mov r0, idle_hook
    st.di r0, [r1]
    jl ROM_USB_ATTACH
    mov r0, 1
    jl ROM_USB_CONFIGURE
    mov r0, 1
    stb r0, [r14, 0x14]
    ld r0, [r14, 4]
    jl ROM_USB_ENDPOINTS
    mov r13, 1
    mov r1, 0xf67a50
    st r13, [r1]
    mov r1, 0xf67a40
    st r13, [r1]
    mov r1, 0xf68278
    st r13, [r1]
    mov r0, 0
    jl ROM_USB_PIPE
    mov r0, 1
    jl ROM_USB_PIPE
    mov r0, 12
    jl ROM_ALLOC
    mov.f r1, r0
    beq warm_restore_done
    mov r2, 0xc1
    stb r2, [r1, 0]
    mov r2, 2
    stb r2, [r1, 1]
    stb r13, [r1, 2]
    mov r2, 0
    stb r2, [r1, 3]
    mov r3, 12
    stb r3, [r1, 4]
    stb r2, [r1, 5]
    stb r2, [r1, 6]
    stb r2, [r1, 7]
    st r2, [r1, 8]
    jl ROM_USB_NOTIFY
warm_restore_done:
    mov r0, 5
    mov r1, ROM_WORK + 8
    st.di r0, [r1]
    pop_s r13
    pop_s r14
    pop_s r15
    ld.ab r16, [sp, 4]
    ld.ab r17, [sp, 4]
    ld.ab r18, [sp, 4]
    ld.ab r19, [sp, 4]
    ld.ab r20, [sp, 4]
    pop_s blink
    j app_init

/* BonDriver_S1UD.dll の J.83B 用 firmware と同じ要求 0x37A (= 890、この probe の命令と同じ番号)。
   90 MHz 以上の引数は周波数 (Hz) として連続送出を始め、送出タスクの作成後の 0 は送出を止める。
   応答 0x37B (= 891) は結果と送出の累積統計 (filled, sent, late, dropped, timeouts, errors) を返す。
   周波数は DriverHost と同じ 90 MHz から 775 MHz までを受け付け、範囲外は結果 1 を返す。
   入口では fixed_data が blink と r13 を退避し、r13 が要求を指す */
iq_request:
    mov r2, 1
    ld r1, [r13, 8]
    breq r1, 0, iq_accept
    mov r3, 775000000
    cmp r1, r3
    bhi iq_reply
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
    /* 固定の PLL 語の試験は、138.75 MHz を副帯域 1、地上波の 13ch と 14ch を副帯域 3 として扱う */
    mov r0, tune_sub
    mov r1, 3
    cmp r6, 0xc0017600
    mov.eq r1, 1
    st r1, [r0]
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
    /* 360 MHz 以下 (副帯域 0 から 2) は、参照 firmware が 138.75 MHz の送出中に使っていた RF の値を書く。
       ゲインは参照の AGC が選んだ値で固定し、周波数ごとの調整はしない */
    mov r0, tune_sub
    ld r1, [r0]
    cmp r1, 3
    bhs prepare_capture_ring
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
    /* RF ゲインの初期値。gain_override が 0 以外ならその値で固定し、AGC を止める。
       0 なら帯域の段列 (360 MHz 未満と以上で別) の開始段を書き、送出中の AGC を有効にする */
    mov r2, agc_state
    mov r0, gain_override
    ld r1, [r0]
    breq r1, 0, prepare_agc
    mov r0, 0x852000
    st.di r1, [r0]
    mov r1, 0
    st r1, [r2, 0]
    b prepare_capture_clear
prepare_agc:
    mov r0, tune_sub
    ld r0, [r0]
    mov r3, agc_low_ladder
    mov r4, AGC_LOW_STEPS
    mov r5, AGC_LOW_START
    cmp r0, 3
    blo prepare_agc_ladder
    mov r3, agc_high_ladder
    mov r4, AGC_HIGH_STEPS
    mov r5, AGC_HIGH_START
prepare_agc_ladder:
    mov r1, 1
    st r1, [r2, 0]
    st r3, [r2, 4]
    st r4, [r2, 8]
    st r5, [r2, 12]
    mov r1, 0
    st r1, [r2, 16]
    st r1, [r2, 20]
    asl r5, r5, 2
    add r5, r5, r3
    ld r1, [r5]
    mov r0, 0x852000
    st.di r1, [r0]
prepare_capture_clear:
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
    mov r0, r2
    jl freq_to_pll
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
    /* r24 は枠の中の何番目のブロックか、r25 は 1 回の送信にまとめるブロック数 */
    mov r24, 0
    mov r0, slot_config
    ld.di r25, [r0, 28]
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
    /* 枠の配置は 2 領域に分かれうるため、送信先は DMA 記述子の先頭語から読む */
    mov r0, slot_config
    ld.di r1, [r0, 12]
    asl r0, r14, 4
    add r0, r0, r1
    ld.di r16, [r0]
    /* 枠の 2 番目のブロックは、1 番目の直後に詰める。枠が使用中かは 1 番目で判定した値を使う */
    breq r24, 0, stream_block_first
    add r16, r16, 14724
    b stream_block_ready
stream_block_first:
    mov r0, slot_busy
    add r0, r0, r14
    ldb.di r19, [r0]
stream_block_ready:
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
    /* 詰めたブロックだけで受信レベルを測る */
    brne r19, 0, stream_agc_done
    jl agc_update
stream_agc_done:
    mov r18, r21
    add r17, r17, 1
    add r24, r24, 1
    cmp r24, r25
    blo stream_block
    mov r24, 0
    add r14, r14, 1
    mov r0, slot_config
    ld.di r1, [r0, 0]
    cmp r14, r1
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
    /* 詰めかけの枠は、最初のブロックから詰め直す */
    mov r24, 0
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
    mov r0, slot_config
    ld.di r2, [r0, 0]
    mov r0, slot_busy
    mov r1, 0
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
    /* まとめる最後のブロックまでは送らない */
    add r0, r24, 1
    cmp r0, r25
    bhs stream_submit_send
    pop_s blink
    j_s [blink]
stream_submit_send:
    mov r0, slot_busy
    add r20, r0, r14
    mov_s r1, 1
    stb.di r1, [r20]
    /* ARC700 の構成に乗算器がない前提で、枠番号 * 12 をシフトで求める */
    asl r2, r14, 3
    asl r3, r14, 2
    add r2, r2, r3
    mov r3, slot_config
    ld.di r3, [r3, 8]
    add r2, r2, r3
    mov_s r0, 0
    mov_s r1, 1
    mov_s r3, 0
    jl ROM_SEND_BULK
    mov r1, stream_stats
    breq r0, 0, stream_submit_reject
    ld.di r2, [r1, 4]
    add r2, r2, r25
    st.di r2, [r1, 4]
    pop_s blink
    j_s [blink]
stream_submit_reject:
    /* 送れなかった枠のブロックはすべて失う。errors は拒否の回数、dropped はブロック数 */
    mov_s r0, 0
    stb.di r0, [r20]
    ld.di r2, [r1, 20]
    add r2, r2, 1
    st.di r2, [r1, 20]
    ld.di r2, [r1, 12]
    add r2, r2, r25
    sub r2, r2, 1
    st.di r2, [r1, 12]
stream_submit_drop:
    mov r1, stream_stats
    ld.di r2, [r1, 12]
    add r2, r2, 1
    st.di r2, [r1, 12]
    pop_s blink
    j_s [blink]

/* 12 枠の HIF ヘッダーと DMA 記述子。参照 firmware の 0x48984 と同じ配置 */
stream_init_slots:
    /* 枠の数が上限を超えていれば上限に揃える */
    mov r5, slot_config
    ld.di r3, [r5, 0]
    cmp r3, STREAM_SLOTS_MAX
    mov.hi r3, STREAM_SLOTS_MAX
    st.di r3, [r5, 0]
    ld.di r2, [r5, 4]
    ld.di r0, [r5, 8]
    ld.di r1, [r5, 12]
    ld.di r6, [r5, 16]
    ld.di r7, [r5, 20]
    ld.di r8, [r5, 24]
    /* 1 回の送信にまとめるブロック数は 1 か 2。HIF の長さは 16 ビットのため 2 までとする */
    ld.di r9, [r5, 28]
    cmp r9, 2
    mov.ne r9, 1
    st.di r9, [r5, 28]
    mov r10, 14724
    cmp r9, 2
    mov.eq r10, 29448
    add r11, r10, 8
    or r11, r11, 0x000d0000
    mov r5, slot_busy
stream_init_next:
    /* 最初の領域に split 枠を置いたら、2 番目の領域の先頭へ移る */
    brne r7, 0, stream_init_place
    mov r2, r8
stream_init_place:
    sub r7, r7, 1
    mov r4, 0x0100037c
    st.di r4, [r0, 0]
    st.di r11, [r0, 4]
    st.di r1, [r0, 8]
    st.di r2, [r1, 0]
    mov r4, 0
    st.di r4, [r1, 4]
    st.di r4, [r1, 8]
    st.di r10, [r1, 12]
    mov r4, 0
    stb.di r4, [r5]
    add r0, r0, 12
    add r1, r1, 16
    add r2, r2, r6
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
    mov r0, rf_band_table
    mov r5, tune_sub
    ld r5, [r5]
    asl r5, r5, 2
pll_mask_next:
    ld r1, [r0, 0]
    breq r1, 0, pll_band_bits
    ld r2, [r0, 4]
    add r3, r0, 8
    add r3, r3, r5
    ld r3, [r3]
    ld.di r4, [r1]
    bic r4, r4, r2
    or r4, r4, r3
    st.di r4, [r1]
    add r0, r0, 24
    b pll_mask_next
pll_band_bits:
    /* 参照 firmware の 0x47c34 と同じく、0x852040 のビット 18 は 360 MHz 以下で立て、ビット 2 は落とす
       (参照が入力の選択に使う値が 0 の場合)。360 MHz を超えると両方を落とす */
    mov r1, 0x852040
    ld.di r4, [r1]
    bclr r4, r4, 2
    bclr r4, r4, 18
    cmp r5, 12
    bhs pll_band_store
    bset r4, r4, 18
pll_band_store:
    st.di r4, [r1]
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
/* 帯域ごとの RF の設定。各行はアドレス、マスク、副帯域 0 から 3 の値。副帯域は 84 MHz 未満、150 MHz 未満、
   360 MHz 未満、それ以上で、参照 firmware の 0x47afc と同じ境界。値は参照 firmware の 0x4ae84 の表と同じで、
   副帯域 1 と 3 の列は実機の 138.75 MHz と 13ch の測定値とも一致した */
rf_band_table:
    .long 0x852040, 0x00000080, 0x00000080, 0x00000080, 0x00000080, 0x00000000
    .long 0x852040, 0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x00000000
    .long 0x852000, 0x00080000, 0x00000000, 0x00000000, 0x00000000, 0x00080000
    .long 0x85202c, 0x0ff00000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852034, 0x00ff0000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852040, 0x00007e00, 0x00007e00, 0x00007e00, 0x00007e00, 0x00002e00
    .long 0x852040, 0x07000000, 0x07000000, 0x02000000, 0x00000000, 0x00000000
    .long 0x852040, 0x00000040, 0x00000000, 0x00000000, 0x00000000, 0x00000040
    .long 0x852040, 0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852040, 0x00020000, 0x00020000, 0x00020000, 0x00020000, 0x00000000
    .long 0x852040, 0x00010000, 0x00000000, 0x00000000, 0x00000000, 0x00010000
    .long 0x852040, 0x00000008, 0x00000000, 0x00000000, 0x00000000, 0x00000008
    .long 0x852040, 0x00000020, 0x00000020, 0x00000020, 0x00000020, 0x00000000
    .long 0x852040, 0x00000010, 0x00000000, 0x00000000, 0x00000000, 0x00000010
    .long 0x852040, 0x00008000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852034, 0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852034, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852040, 0x00000001, 0x00000001, 0x00000001, 0x00000001, 0x00000000
    .long 0x852004, 0x00000080, 0x00000000, 0x00000000, 0x00000000, 0x00000080
    .long 0x852040, 0x00400000, 0x00400000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852040, 0x40000000, 0x40000000, 0x40000000, 0x40000000, 0x40000000
    .long 0x852040, 0x00080000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
    .long 0x852008, 0x00000300, 0x00000200, 0x00000200, 0x00000200, 0x00000000
    .long 0
/* 選局中の副帯域 (0 から 3) */
tune_sub:
    .long 3
/* 0x852000 へ書く RF ゲインの指定 (0 は AGC) */
gain_override:
    .long 0
/* AGC の状態: 有効, 段列, 段数, 現在の段, 積算したブロック数, 振幅の積算 */
agc_state:
    .long 0, 0, 0, 0, 0, 0
/* RF ゲインの段列 (0x852000 の値)。弱い側から強い側へ並べる。いずれも参照 firmware の AGC が送出中に
   選んでいた値で、327.25 MHz と 333.75 MHz と 138.75 MHz で RMS がこの順に増えた (隣の段との比は約 1.3 から 2 倍)。
   360 MHz 以上は 580.75 MHz で同じく順序を確かめた */
    .equ AGC_LOW_STEPS, 10
    .equ AGC_LOW_START, 1
agc_low_ladder:
    .long 0x001585d8, 0x00108d59, 0x001309d9, 0x001409d9, 0x001585dc
    .long 0x001409dc, 0x0010915c, 0x001209df, 0x001409df, 0x00159959
    .equ AGC_HIGH_STEPS, 5
    .equ AGC_HIGH_START, 2
agc_high_ladder:
    .long 0x001d8a5f, 0x001a09f2, 0x001c897a, 0x001909fa, 0x001d9a5f
/* 32 ブロックごとの振幅の積算に対する閾値 (下限, 上限)。各ブロックから 64 語 (256 標本) の絶対値を足す。
   ガウス雑音に近い信号では、平均絶対値 14 と 32 が複素 RMS の約 25 と 57 に当たる。
   下限を平均絶対値 11 (RMS 約 19) にすると、弱いチャンネルが RMS 20 前後で止まり、MER が参照より 1.5 から 2 dB
   下がった (333.75 MHz で 27.9 dB)。参照 firmware は RMS 35 前後に保っている。隣の段との比は最大 2 倍のため、
   上限と下限の比を 2 倍より大きくして段の間を行き来しないようにする。送出前なら引数 100 の書き込みで変えられる */
    .equ AGC_BLOCKS, 32
agc_limits:
    .long 14 * 256 * 32, 32 * 256 * 32

/* r16 のブロックから受信レベルを測り、AGC_BLOCKS ごとに RF ゲインを 1 段上げ下げする */
agc_update:
    mov r2, agc_state
    ld r0, [r2, 0]
    breq r0, 0, agc_return
    /* 14720 バイトのブロック全体から間隔を空けて 64 語を取る */
    mov r0, r16
    mov r1, 64
    ld r3, [r2, 20]
agc_measure:
    ld.ab r4, [r0, 228]
    sexb r5, r4
    abs r5, r5
    add r3, r3, r5
    asr r5, r4, 8
    sexb r5, r5
    abs r5, r5
    add r3, r3, r5
    asr r5, r4, 16
    sexb r5, r5
    abs r5, r5
    add r3, r3, r5
    asr r5, r4, 24
    abs r5, r5
    add r3, r3, r5
    sub.f r1, r1, 1
    bne agc_measure
    st r3, [r2, 20]
    ld r0, [r2, 16]
    add r0, r0, 1
    st r0, [r2, 16]
    cmp r0, AGC_BLOCKS
    blo agc_return
    mov r0, 0
    st r0, [r2, 16]
    st r0, [r2, 20]
    ld r1, [r2, 12]
    mov r5, agc_limits
    ld r4, [r5, 0]
    cmp r3, r4
    bhs agc_check_high
    ld r0, [r2, 8]
    sub r0, r0, 1
    cmp r1, r0
    bhs agc_return
    add r1, r1, 1
    b agc_apply
agc_check_high:
    ld r4, [r5, 4]
    cmp r3, r4
    bls agc_return
    breq r1, 0, agc_return
    sub r1, r1, 1
agc_apply:
    st r1, [r2, 12]
    ld r0, [r2, 4]
    asl r1, r1, 2
    add r0, r0, r1
    ld r1, [r0]
    mov r0, 0x852000
    st.di r1, [r0]
agc_return:
    j_s [blink]


/* r0 の周波数 (Hz) から PLL 語を r6 へ求め、副帯域を tune_sub へ書く。参照 firmware の 0x47afc と 0x478b4 と同じ計算で、
   12 MHz の基準に対する刻みは 84 MHz 未満が 1/16、360 MHz 未満が 1/8、それ以上が 2 倍。
   N = round(f / 刻み) (半分は切り上げ)、frac = round((f - N * 刻み) * 8192 / 刻み) を符号つき 14 ビットで置く。
   下位 10 ビットは帯域表の 0x852008 の値 (360 MHz 未満は 0x200) */
freq_to_pll:
    mov r2, 3
    mov r3, 24000000
    mov r4, 359999999
    cmp r0, r4
    bhi freq_band_done
    mov r2, 2
    mov r3, 1500000
    mov r4, 149999999
    cmp r0, r4
    bhi freq_band_done
    mov r2, 1
    mov r4, 83999999
    cmp r0, r4
    bhi freq_band_done
    mov r2, 0
    mov r3, 750000
freq_band_done:
    mov r4, tune_sub
    st r2, [r4]
    mov r4, 0
    mov r5, r0
freq_div:
    cmp r5, r3
    blo freq_div_done
    sub r5, r5, r3
    add r4, r4, 1
    b freq_div
freq_div_done:
    asl r7, r5, 1
    cmp r7, r3
    blo freq_frac
    add r4, r4, 1
    sub r5, r5, r3
freq_frac:
    mov r8, 0
    brge r5, 0, freq_frac_abs
    neg r5, r5
    mov r8, 1
freq_frac_abs:
    mov r9, 0
    mov r10, 13
freq_frac_loop:
    asl r5, r5, 1
    asl r9, r9, 1
    cmp r5, r3
    blo freq_frac_next
    sub r5, r5, r3
    bset r9, r9, 0
freq_frac_next:
    sub.f r10, r10, 1
    bne freq_frac_loop
    asl r7, r5, 1
    cmp r7, r3
    blo freq_frac_rounded
    add r9, r9, 1
freq_frac_rounded:
    breq r8, 0, freq_word
    neg r9, r9
freq_word:
    and r9, r9, 0x3fff
    asl r9, r9, 18
    and r4, r4, 0xff
    asl r4, r4, 10
    or r6, r9, r4
    cmp r2, 3
    bhs freq_done
    or r6, r6, 0x200
freq_done:
    j_s [blink]

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
    /* 送出の枠のヘッダーなら、その枠を空きに戻す。枠 0 は従来の単発送信と共用する */
    mov r2, slot_config
    ld.di r4, [r2, 0]
    asl r3, r4, 3
    asl r4, r4, 2
    add r4, r4, r3
    ld.di r2, [r2, 8]
    sub r3, r1, r2
    mov r2, SAMPLE_HEADER
    cmp r3, r4
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
/* warm 入口で 0xe40000 へ写す領域 (写し先, 写し元, 長さ)。写し元が 0xf00000 以上なら、その ROM の変数が指す領域。
   小さい値は 0xf67a30 が指す構造の中の、その位置の語が指す領域 */
usb_save_table:
    .long USB_SAVE + 0x14, 0xf67a24, 0x20
    .long USB_SAVE + 0x34, 0xf67a28, 0x50
    .long USB_SAVE + 0x84, 0xf67a2c, 0x38
    .long USB_SAVE + 0xbc, 0xf67a30, 404
    .long USB_SAVE + 0x250, 0xf67a44, 0x20
    .long USB_SAVE + 0x270, 0xf67a34, 0x40
    .long USB_SAVE + 0x2b0, 0x30, 0x28
    .long USB_SAVE + 0x2d8, 0x34, 2
    .long 0
/* 復元処理で ROM の USB の処理表へ置く値 (アドレス, 値) */
usb_rom_words:
    .long 0xf67f8c, 0x6788, 0xf67f90, 0x6798, 0xf67f98, 0x67c0, 0xf67f9c, 0x67a8
    .long 0xf67f54, 0x63e0, 0xf67f58, 0x64a0, 0xf67f5c, 0x64e4, 0xf67f60, 0x6648, 0xf67f64, 0x66d8
    .long 0xf67f68, 0, 0xf67f6c, 0, 0xf67a58, 0, 0xf68278, 0, 0xf68274, 1
    .long 0
small_frame:
    .short 0
    .byte 11, 0
    .short 12, 0
    .long 0
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
/* 送信枠の数、最初の領域の先頭、HIF ヘッダーの先頭 (12 バイト間隔)、DMA 記述子の先頭 (16 バイト間隔)、
   枠の間隔、最初の領域に置く枠の数、2 番目の領域の先頭、1 回の送信にまとめるブロック数 (1 か 2)。
   送出を始める前なら、引数 100 の書き込みで変えられる。
   既定は参照 firmware と同じ 0xe20000 からの 0x4000 間隔の 12 枠で、1 ブロックずつ送る */
slot_config:
    .long 12, SAMPLE_PAYLOAD, SAMPLE_HEADER, SAMPLE_DESCRIPTOR, 0x4000, 0xffffffff, 0, 1
slot_busy:
    .space STREAM_SLOTS_MAX
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
