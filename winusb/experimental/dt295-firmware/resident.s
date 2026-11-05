/* SPDX-License-Identifier: GPL-2.0-only */
/* DT295向けの独立したプロトコルコア。USB入口の接続は別の実装段階。 */
        .section .text
        .global dtp_entry
dtp_entry:
        /* r4は呼出側が所有する、4-byte整列した64-byteパケット。 */
        cmp #0,r4
        beq .Lreturn
        ld @r4,r0
        ldi:32 #0x50494631,r1 /* PIF1: 既存常駐方式との誤接続を検出する。 */
        cmp r1,r0
        bne .Lbad_magic
        mov r4,r1
        addn #12,r1
        ld @r1,r0
        cmp #1,r0
        bne .Lbad_version
        mov r4,r1
        addn #8,r1
        ld @r1,r0
        cmp #1,r0
        beq .Lok
        cmp #2,r0
        beq .Lok
        cmp #3,r0
        beq .Lnot_ready
        ldi:8 #3,r2
        bra .Lreply
.Lbad_magic:
        ldi:8 #1,r2
        bra .Lreply
.Lbad_version:
        ldi:8 #2,r2
        bra .Lreply
.Lnot_ready:
        /* I2Cコントローラーが未検証のため、転送を成功扱いにしない。 */
        ldi:8 #4,r2
        bra .Lreply
.Lok:
        ldi:8 #0,r2
.Lreply:
        /* エラー時も要求ペイロードを応答へ残さない。MMIO操作は不要。 */
        mov r4,r1
        addn #12,r1
        st r2,@r1
        addn #4,r1
        ldi:8 #1,r0
        st r0,@r1
        addn #4,r1
        ldi:8 #0,r0
        st r0,@r1
        addn #4,r1
        ldi:8 #10,r3
.Lclear:
        st r0,@r1
        addn #4,r1
        addn2 #-1,r3
        cmp #0,r3
        bne .Lclear
        cmp #0,r2
        bne .Lheader
        mov r4,r1
        addn #8,r1
        ld @r1,r0
        cmp #1,r0
        bne .Lheader
        /* CAPS: I2C/TSの機能ビットは実装・検証完了まで0。 */
        mov r4,r1
        addn #12,r1
        addn #8,r1
        ldi:8 #16,r0
        st r0,@r1
        addn #8,r1
        ldi:8 #40,r0
        st r0,@r1
        addn #4,r1
        st r0,@r1
        addn #4,r1
        ldi:32 #0x06b8103b,r0
        st r0,@r1
.Lheader:
        ldi:32 #0x50494631,r0
        st r0,@r4
        mov r4,r1
        addn #8,r1
        ld @r1,r0
        ldi:32 #0x80000000,r2
        or r2,r0
        st r0,@r1
.Lreturn:
        ret
