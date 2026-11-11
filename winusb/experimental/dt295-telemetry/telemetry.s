/* SPDX-License-Identifier: GPL-2.0-only */
/* mAgicVISION 0.1.1専用の計測パッチ。送出予算・RF設定の変更はない。 */
.section .text
.global telemetry_dispatch, telemetry_drain, telemetry_fifo, telemetry_pointer
telemetry_dispatch:
 mov r7,r0
 addn #12,r0
 ld @r0,r0
 ldi:8 #0x24,r1
 cmp r1,r0
 bne passthrough
 ldi:32 #0x803adfe4,r0
 cmp r0,r5
 bne passthrough
 st r8,@-r15
 st r9,@-r15
 st r10,@-r15
 st r11,@-r15
 st rp,@-r15
 addsp #-48
 mov r7,r8
 mov r15,r10
 mov r15,r0
 ldi:8 #0,r1
 ldi:8 #10,r2
clear:
 st r1,@r0
 addn #4,r0
 addn2 #-1,r2
 cmp #0,r2
 bne clear
 ldi:32 #telemetry_pointer,r11
 st r10,@r11
 ldi:32 #0x01902028,r0
 ld @r0,r1
 st r1,@(r15,4)
 addn #4,r0
 ld @r0,r1
 st r1,@(r15,8)
 ldi:32 #0x01902024,r0
 ld @r0,r1
 st r1,@(r15,32)
 ldi:32 #0x80381d0c,r0
 call @r0
 mov r4,r9
 ldi:32 #0x01902028,r0
 ld @r0,r1
 st r1,@(r15,12)
 addn #4,r0
 ld @r0,r1
 st r1,@(r15,16)
 ldi:32 #0x44545331,r1
 st r1,@r15
 mov r15,r0
 mov r8,r1
 ldi:8 #24,r2
 addn r2,r1
 ldi:8 #10,r2
copy:
 ld @r0,r3
 st r3,@r1
 addn #4,r0
 addn #4,r1
 addn2 #-1,r2
 cmp #0,r2
 bne copy
 ldi:8 #0,r0
 st r0,@r11
 mov r9,r4
 addsp #48
 ld @r15+,rp
 ld @r15+,r11
 ld @r15+,r10
 ld @r15+,r9
 ld @r15+,r8
 ret
passthrough:
 ldi:32 #0x80381d0c,r0
 jmp @r0
.balign 4
telemetry_drain:
 /* 64版は元の44-byteループと一致。128/256版の許容差分も生成時に検査する。 */
 .short 0x8fe0,0x1781,0xa3ff
 st r4,@r15
 ldi:20 #0x1000,r9
 ldi:8 #0,r8
 ldi:32 #0x80399668,r10
again:
 ld @r15,r4
 call @r10
 cmp #0,r4
 blt counted
 bne budget
 addn #1,r8
budget:
 addn2 #-1,r9
 cmp #0,r9
 beq counted
 .if DRAIN_LIMIT < 256
 ldi:8 #DRAIN_LIMIT,r1
 .else
 ldi:20 #DRAIN_LIMIT,r1
 .endif
 cmp r1,r8
 bne again
counted:
 ldi:32 #telemetry_pointer,r0
 ld @r0,r0
 cmp #0,r0
 beq drained
 ldi:20 #0x1000,r1
 subn r9,r1
 cmp #0,r4
 bge attempted
 addn #1,r1
attempted:
 ldi:8 #20,r2
 addn r0,r2
 ld @r2,r3
 addn r1,r3
 st r3,@r2
 addn #4,r2
 ld @r2,r3
 addn r8,r3
 st r3,@r2
drained:
 .short 0xa301,0x0781,0x8d07,0x9720
.balign 4
telemetry_fifo:
 st r8,@-r15
 st rp,@-r15
 addsp #-8
 ld @(r15,16),r0
 st r0,@r15
 ld @(r15,20),r0
 st r0,@(r15,4)
 ldi:32 #0x8038328c,r0
 call @r0
 ldi:32 #telemetry_pointer,r8
 ld @r8,r8
 cmp #0,r8
 beq fifo_done
 ldi:8 #36,r0
 addn r8,r0
 ld @r0,r1
 addn #1,r1
 st r1,@r0
 cmp #1,r4
 bne fifo_done
 ldi:8 #28,r0
 addn r8,r0
 ld @r0,r1
 addn #1,r1
 st r1,@r0
fifo_done:
 addsp #8
 ld @r15+,rp
 ld @r15+,r8
 ret
.balign 4
telemetry_pointer:
 .long 0
