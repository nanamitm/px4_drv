# POSIX IPC 回帰試験

この試験は socketpair を使い、USB デバイスなしで macOS 用の通信実装を検証します。macOS と Linux の C++17・CMake 環境が対象です。

```sh
cmake -S macos/tests -B macos/build/ipc-tests
cmake --build macos/build/ipc-tests
ctest --test-dir macos/build/ipc-tests --output-on-failure
```

制御接続は既存のコマンドコードと要素数から要求長を計算し、データ接続は固定長の DataCmd を蓄積します。送信する構造体・版番号・クライアントの通信形式に変更はなく、従来の px4rec と接続できます。容量を超える要素数や途中切断では接続を終了します。

試験対象は分割受信、連結された要求の境界、可変長要素数、途中切断、送信待機のキャンセルです。実機の受信、macOS 固有の USB・フレームワーク、長時間録画は別途確認が必要です。
