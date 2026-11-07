/* SPDX-License-Identifier: GPL-2.0-only */
/* DT295の既存BC OUTハンドラーへ接続する診断用アダプター。 */
        .section .text
        .global dtp_usb_hook
dtp_usb_hook:
        /* r11は既存ブート処理の要求構造体。境界条件は解析で確認した版に限定。 */
        ldi:8 #0x46,r13
        ldub @(r13,r11),r0
        ldi:8 #0x7e,r1
        cmp r1,r0
        bne .Lstock
        ldi:8 #0x48,r13
        lduh @(r13,r11),r0
        ldi:8 #64,r1
        cmp r1,r0
        bne .Lstock
        ld @r11,r0
        ldi:32 #0x000c4620,r1
        cmp r1,r0
        bne .Lstock
        /* 受信データは構造体+4から明示コピーし、前回のRAM内容には依存しない。 */
        mov r11,r5
        addn #4,r5
        mov r1,r4
        mov r1,r6
        ldi:8 #16,r7
.Lcopy:
        ld @r5,r0
        st r0,@r6
        addn #4,r5
        addn #4,r6
        addn2 #-1,r7
        cmp #0,r7
        bne .Lcopy
        ldi:32 #0x000e1100,r12
        call @r12
        /* 応答はstagingへ残す。EP0完了と保存レジスター復帰は既存処理へ戻る。 */
        ldi:32 #0x000ea8c8,r12
        jmp @r12
.Lstock:
        /* 上書きする8 bytesの判定と同じ分岐をソースで表現する。 */
        ldi:8 #0x46,r13
        ldub @(r13,r11),r0
        cmp #1,r0
        bne .Lordinary
        ldi:32 #0x000ea704,r12
        jmp @r12
.Lordinary:
        ldi:32 #0x000ea75a,r12
        jmp @r12

        .section .hook,"ax"
        .global dtp_boot_patch
dtp_boot_patch:
        ldi:20 #dtp_usb_hook,r12
        jmp @r12
        nop
