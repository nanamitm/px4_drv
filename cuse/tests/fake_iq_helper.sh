#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# iq_process_test の模擬の helper。msi2500_frontend serve-s1ud と同じフレーム (rtlsdr/protocol.hpp) を標準出力へ書く。
# フレームの先頭は 24 バイトで、magic (RST1)、kind、size、quality、accepted、rejected の 32 ビット (リトルエンディアン)。
# FAKE_IQ_HELPER の値で動作を変える。
#   normal  準備完了、TS 1 パケット、MER 31.50 dB の状態を書き、標準入力の終わりで終了する
#   crash   準備完了を書いた直後に異常終了する
#   bad     形式の異常なフレームを書く
#   hang    標準入力の終わりを無視して終了しない (停止の期限で強制終了される)
frame() {
    # $1 kind、$2 と $3 は size の下位 2 バイト、$4 は quality の下位 1 バイト (どれも 8 進数の 3 桁)
    printf '\122\123\124\061'
    printf "\\$1\\000\\000\\000"
    printf "\\$2\\$3\\000\\000"
    printf "\\$4\\000\\000\\000"
    printf '\000\000\000\000\000\000\000\000'
}
case "${FAKE_IQ_HELPER:-normal}" in
normal)
    frame 001 000 000 000
    # TS (kind 2、188 バイト = 0274): 0x47 0x00 0x00 0x10 と 184 バイトの 0。quality は 100 (0144)
    frame 002 274 000 144
    printf '\107\000\000\020'
    head -c 184 /dev/zero
    # 状態 (kind 3、4 バイト): MER 3150 (0x0c4e)
    frame 003 004 000 144
    printf '\116\014\000\000'
    cat >/dev/null
    ;;
crash)
    frame 001 000 000 000
    exit 3
    ;;
bad)
    printf 'not a helper frame at all'
    cat >/dev/null
    ;;
hang)
    frame 001 000 000 000
    exec 0</dev/null
    sleep 60
    ;;
esac
