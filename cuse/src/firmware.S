/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PIX-DT295 の組み込みファームウェアを実行ファイルへ埋め込む。Windows 版の EXE のリソース
 * (winusb/src/DriverHost_PX4/DriverHost_PX4.rc の IDR_DT295_*_FIRMWARE) と同じファイルを使い、
 * 利用者がファイルを別に配置する手順を不要にする。.incbin の検索先は Makefile が -Wa,-I で渡す。
 */
	.section .rodata
	.balign 16
	.global px4_dt295_direct_fr
	.global px4_dt295_direct_fr_end
px4_dt295_direct_fr:
	.incbin "dt295-direct-fr.bin"
px4_dt295_direct_fr_end:

	.balign 16
	.global px4_dt295_source_fr
	.global px4_dt295_source_fr_end
px4_dt295_source_fr:
	.incbin "dt295-source-fr.bin"
px4_dt295_source_fr_end:

	.section .note.GNU-stack,"",%progbits
