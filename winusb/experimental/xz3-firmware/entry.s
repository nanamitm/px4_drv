/* SPDX-License-Identifier: GPL-2.0-only */
        .section .text.entry
        .global xz_usb_entry
xz_usb_entry:
        /* ブートROMの通常読み出しと同じアドレス・長さを参照する。 */
        ldi:8 #0x44,r13
        ld @(r13,r8),r0
        ldi:20 #0xc4600,r12
        cmp r12,r0
        bne .Lstock
        ldi:8 #6,r13
        lduh @(r13,r11),r0
        ldi:8 #64,r13
        cmp r13,r0
        bne .Lstock
        ld @r12,r0
        ldi:32 #0x585a4931,r13
        cmp r13,r0
        bne .Lstock

        st rp,@-r15
        st r1,@-r15
        st r2,@-r15
        st r3,@-r15
        st r5,@-r15
        st r6,@-r15
        st r7,@-r15
        mov r12,r4
        ldi:32 #xz_i2c_relay,r12
        call @r12
        ld @r15+,r7
        ld @r15+,r6
        ld @r15+,r5
        ld @r15+,r3
        ld @r15+,r2
        ld @r15+,r1
        ld @r15+,rp
        /* 64バイトのコピーが完了したときのROM側のレジスターを再現する。 */
        ldi:20 #0xc4600,r8
        ldi:8 #64,r4
        mov r4,r0
        ldi:8 #6,r13
        ldi:20 #0xe9a66,r12
        jmp @r12

.Lstock:
        /* フックで上書きした最初の8バイトを再現して元の比較命令へ戻る。 */
        ldi:8 #0x44,r13
        ld @(r13,r8),r4
        ldi:20 #0x80000,r0
        ldi:20 #0xe9a18,r12
        jmp @r12

        .section .hook,"ax"
        .global xz_boot_hook
xz_boot_hook:
        ldi:20 #xz_usb_entry,r12
        jmp @r12
        nop
