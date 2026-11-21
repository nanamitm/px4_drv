/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PIX-DT295 の組み込みファームウェアと GV-MVP/XZ3 の中継プログラムを実行ファイルへ埋め込む。
 * Windows 版の EXE のリソース (winusb/src/DriverHost_PX4/DriverHost_PX4.rc の IDR_DT295_*_FIRMWARE と
 * IDR_XZ3_*) と同じファイルを使い、利用者がファイルを別に配置する手順を不要にする。
 * .incbin の検索先は Makefile が -Wa,-I で渡す。
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

	/* GV-MVP/XZ3 の中継プログラムは 4 バイト単位で RAM へ書くため、境界を揃える。 */
	.balign 16
	.global px4_xz3_bootstrap
	.global px4_xz3_bootstrap_end
px4_xz3_bootstrap:
	.incbin "xz3-bootstrap.bin"
px4_xz3_bootstrap_end:

	.balign 16
	.global px4_xz3_runtime
	.global px4_xz3_runtime_end
px4_xz3_runtime:
	.incbin "xz3-runtime.bin"
px4_xz3_runtime_end:

	.balign 16
	.global px4_xz3_hook
	.global px4_xz3_hook_end
px4_xz3_hook:
	.incbin "xz3-hook.bin"
px4_xz3_hook_end:

	.section .note.GNU-stack,"",%progbits
