# 受信・通信終了の回帰試験

以下は実機を使わない試験です。Windows は Visual Studio の v143 toolset、POSIX IPC は C++17 と CMake、Linux のコピー失敗注入は Python 3 と C コンパイラーを使用します。

## Windows

`winusb/build.ps1` に以下の試験を組み込み、x86 と x64 の両方で実行します。個別実行する場合は、対象の vcxproj を Release-static でビルドし、`winusb/build/<architecture>/Release-static/` の実行ファイルを起動できます。

| 試験 | 検証する条件 |
| --- | --- |
| stream_stop_test | 読み出し開始より先の停止、無入力時の停止競合、送信先が詰まっている間の通知と停止 |
| pipe_shutdown_test | 未読応答を残した接続破棄、送信バッファが詰まった状態でのキャンセル |
| card_status_wait_test | BUSY 応答中のタイムアウト0・有限期限・キャンセルと、BUSY 解除後の状態通知 |

パイプ試験はプロセス固有の名前を使います。カード監視試験は実際の WinSCard 実装へ模擬応答を注入するため、DriverHost や実機カードへの接続は不要です。

## Linux

```sh
python3 driver/tests/test_ringbuffer_read.py
```

現行の `ringbuffer_read_user()` を取り出し、`copy_to_user()` の失敗を注入してコンパイル・実行します。折り返し前後の失敗、コピー済みバイト、残量、読み出し位置を検査します。原子操作の代替は単一スレッド用であり、カーネル内の並行動作は対象外です。カーネルモジュール自体のビルドは、対象カーネルの完全なヘッダー一式を用意して別途行います。

## macOS 用 POSIX 通信

[IPC 試験の手順](../macos/tests/README.md) を参照してください。Linux 上での試験はソケット通信部分の検証であり、macOS ネイティブのビルド・USB 動作確認とは区別が必要です。

## 実機での確認

録画への影響を判断するには、対象製品・アンテナ・カードを用いてチャンネル切り替え、USB 抜去、受信とカード通信の同時実行、長時間受信を試験し、同期エラー・TEI・continuity error・空受信時間・APDU 失敗数を記録する必要があります。上記の回帰試験だけでは、この実機確認を代替できません。
