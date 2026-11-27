#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# msi2500_ops_test の模擬の helper。msi2500_frontend ops と同じく、制御転送の列を 1 行ずつ標準出力へ書く。
# FAKE_MSI2500_OPS の値で動作を変える。
#   normal       列を書いて終了コード 0 で終わる
#   linger       列を書いて標準出力を閉じ、0.3 秒後に終了コード 0 で終わる (EOF の後に回収できるまで間がある正常終了)
#   fail         列を書いて終了コード 1 で終わる
#   hang         列を書いて標準出力を開いたまま終了しない (読み出しの期限で強制終了される)
#   closed_hang  列を書いて標準出力を閉じ、終了しない (回収の期限で強制終了される)
ops() {
    printf 'setup 43 0 0\n'
    printf 'setup 9 abcd 1234\n'
    printf 'stop 45 0 0\n'
}
case "${FAKE_MSI2500_OPS:-normal}" in
normal)
    ops
    ;;
linger)
    ops
    exec >&-
    sleep 0.3
    ;;
fail)
    ops
    exit 1
    ;;
hang)
    ops
    exec sleep 60
    ;;
closed_hang)
    ops
    exec >&-
    exec sleep 60
    ;;
*)
    exit 2
    ;;
esac
