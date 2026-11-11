/* SPDX-License-Identifier: GPL-2.0-only */
        .section .startup,"ax"
        .global dtp_aux_entry
dtp_aux_entry:
        /* 参考コードと同じFR入口形式。実機CPU状態との適合は未検証。 */
        andccr #0xef
        ldi:32 #__stack_top,r15
        ldi:32 #__bss_start,r0
        ldi:32 #__bss_end,r1
        ldi:8 #0,r2
.Lclear:
        cmp r1,r0
        beq .Lstart
        st r2,@r0
        addn #4,r0
        bra .Lclear
.Lstart:
        ldi:32 #dtp_aux_main,r12
        call @r12
.Lhalt:
        bra .Lhalt
