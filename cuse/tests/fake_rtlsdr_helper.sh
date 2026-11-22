#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# rtlsdr_helper_test の模擬の helper。rtlsdr_frontend --stdin と同じフレーム (rtlsdr/protocol.hpp) を標準出力へ書く。
# フレームの先頭は 24 バイトで、magic (RST1)、kind、size、quality、accepted、rejected の 32 ビット (リトルエンディアン)。
# FAKE_RTLSDR_HELPER の値で動作を変える。
#   normal  準備完了を書き、標準入力の I/Q を 1000 バイト読んでから TS 1 パケットと MER 13.53 dB の状態を書き、
#           標準入力の終わりで終了する
#   crash   準備完了を書いた直後に終了コード 3 で終わる
#   hang    標準入力を閉じ、終わりも無視して終了しない (停止の期限で強制終了される)
#   stall   標準入力を開いたまま読まずに終了しない (標準入力のキューがあふれる)
frame() {
    # $1 kind、$2 と $3 は size の下位 2 バイト、$4 は quality の下位 1 バイト (どれも 8 進数の 3 桁)
    printf '\122\123\124\061'
    printf "\\$1\\000\\000\\000"
    printf "\\$2\\$3\\000\\000"
    printf "\\$4\\000\\000\\000"
    printf '\000\000\000\000\000\000\000\000'
}
case "${FAKE_RTLSDR_HELPER:-normal}" in
normal)
    frame 001 000 000 000
    head -c 1000 >/dev/null
    # TS (kind 2、188 バイト = 0274): 0x47 0x1f 0xff 0x10 と 184 バイトの 0。quality は 100 (0144)
    frame 002 274 000 144
    printf '\107\037\377\020'
    head -c 184 /dev/zero
    # 状態 (kind 3、4 バイト): MER 1353 (0x0549)
    frame 003 004 000 144
    printf '\111\005\000\000'
    cat >/dev/null
    ;;
crash)
    frame 001 000 000 000
    exit 3
    ;;
hang)
    frame 001 000 000 000
    exec 0</dev/null
    sleep 60
    ;;
stall)
    frame 001 000 000 000
    sleep 60
    ;;
esac
