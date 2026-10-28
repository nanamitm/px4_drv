# CECH-ZD1J 統合版の保守情報

## 実装の由来

CECH-ZD1J の機種別制御、192 バイトから 188 バイトへの TS 変換、内蔵カードリーダー、
WinUSB INF、配布レイアウトは、n0atl/px4_drv の `feature/torne-winusb`、
コミット `a3608026b353fcd502590dd260ea216ade325608` の差分を基にしています。
元の変更は `678e3c8`、`cefb385`、`a360802` の 3 コミットです。

既存の DriverHost_PX4 が USB の所有権を持ち、BonDriver と WinSCard からの要求を仲介します。
CECH-ZD1J を複数接続した場合も ReceiverManager と CardServer が管理するため、
DriverHost_CECHZD1J の別プロセスや名前付きパイプを追加する必要はありません。
同じ個体を複数のホストから開くと共有管理が成立しないため、利用するホストを統一してください。

このリポジトリの CardDevice はボーレートを bps の整数で受け取るため、移植元の
IT930x 列挙型から `std::uint32_t` へ適合しています。CECH-ZD1J の対応値は 19200 bps です。
BonDriver の診断には既存の `DebugLog` / `DebugLogPath` を利用できます。

## 検証範囲

### TS の世代とプロトコル互換性

`BonDriver_Torne.ini` の `FramedStream=true` はプロトコル `0x00040004` 以降を使用します。
制御パイプの `GET_VERSION` で対応を確認してから、データパイプへ `SET_DATA_ID_FRAMED=2` を送ります。
非対応ホストでは、バイト列を TS と誤認しないようチューナーのオープンが失敗します。
ヘッダーは little endian の magic `0x31535850`、32bit 世代、32bit バイト数の計 12 バイトです。
TS 本体は 188 バイト単位で最大 `188*4096` バイトとなり、ヘッダーは BonDriver 内で除去されます。

`SET_DATA_ID=1` の従来形式と既存の構造体配置は維持されています。
既存機種の INI は `FramedStream=false` 相当のため、従来の TS 経路を使用します。
新しいホストでも旧 BonDriver の raw 接続を受け付けますが、
自動復帰時の古い TS キュー破棄には新しい BonDriver と `FramedStream=true` が必要です。
世代変更で送信中のバイト列まで新世代と扱うことを防ぐため、
ホストでは ring から読み出したバイト列と世代を同じ短いロック範囲で確定します。
BonDriver は旧キューと作成途中のバッファーを破棄し、アプリへ返したポインターは次の呼び出しまで保持します。

coax の復調器と core-1 プログラムは `471328e03942219544e78004e961d8f69d27f46f`
へ更新しています。周波数別の TMCC と capacitor trim はブリッジ RAM の 64 エントリーへ保存され、
同じテーブル形式であれば再オープン後も保持されます。形式不一致時は初期化が必要です。
ジョブ完了が確認できなかったバスは無効になるため、古い応答を次の要求へ流用することはありません。

`tests/check_torne_native.py --arch x86` / `--arch x64` は独立した Go 模擬バスと C++ の
I2C・待機・ジョブ呼び出しを 9 条件で比較します。
`tools/torne_codegen/compare_upstream.py --coax <pinned-checkout>` は、
元の coax パッケージと抽出した生成器の core プログラムが一致することを確認する試験です。

自動試験では USB を開かず、TS 境界、カード状態遷移、模擬メモリ転送、
Go と C++ の I2C・待機・結果の一致を確認できます。
実機未接続の環境では、受信品質、複数台の同時受信、カード通信中のドロップ、
USB 抜去、TVTest の映像復号、EDCB の長時間録画は未確認です。
移植元の実測結果と本統合版の検証結果は区別する必要があります。
