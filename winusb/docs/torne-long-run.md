# torne CECH-ZD1J 長時間受信（2026-11-12）

TVTest で CECH-ZD1J を約2時間20分連続して受信し、DriverHost の受信段と BonDriver の出力で TS の欠落を記録した結果です。
現行の統合版を配布設定のまま使い、`CECH-ZD1J.inf` で導入した環境での初めての長時間受信です。

この文書の「検証環境」は次の構成を指します。

- Windows 11 Pro x64（10.0.26300）、USB で接続した CECH-ZD1J（`054c:04b2`）1台と地デジアンテナ
- `CECH-ZD1J.inf`（MEDIA 分類、px4_drv CA の署名、Driver Version 1.0.0.1）を `pnputil /add-driver ... /install` で導入
- 64ビット版 TVTest と同じフォルダーに、develop `a1ee070` からビルドした x64 版の `BonDriver_Torne.dll`、`DriverHost_PX4.exe`、`WinSCard.dll` を配置
- `DriverHost_PX4.ini` の torne の定義は配布 INI と同じ値（`DeviceId="auto"`、`I2cBackend="native"`、`OutputErrorPackets=false`、`BulkI2c=true`、`AutoRecovery=false`、`InitialLockRetry=false`）
- `BonDriver_Torne.ini` は配布 INI から `DebugLog=true` だけを変更
- 同じ DriverHost に PIX-DT295 も接続し、その受信機は使っていない

CECH-ZD1J はシリアル番号を持たないため、DriverHost の機器 ID（`bd7f80c5067cfcf8`）は USB の接続位置から作られます。

## チャンネルスキャン

TVTest の地デジのスキャンで、13〜17ch（473.143〜497.143 MHz）を受信できました。
各チャンネル約2秒の受信で、DriverHost の受信段と BonDriver の出力のどちらでも TEI、CC の異常、形式不正は0件でした。
選局直後に188バイトの境界を探す間に捨てたデータ（`discarded`）は23〜133パケットで、BonDriver の出力には含まれません。
18ch（503.143 MHz）から34ch（599.143 MHz）は、いずれも約0.6秒で検索失敗を確定しました。35ch 以上はスキャンしていません。

## 結果

地デジ 17ch（497.143 MHz）を 16:31:00〜18:52:44（2時間21分）受信しました。

| 項目 | DriverHost の受信段 | BonDriver の出力 |
|---|---:|---:|
| パケット | 97,639,935 | 97,638,400 |
| TEI | 0 | 0 |
| CC の異常 | 0 | 0 |
| 形式不正 | 0 | 0 |
| 同期、再同期で捨てたバイト、重複 | （集計の対象外） | 0 |
| 出力の最大の空き時間（`max_gap_us`） | （集計の対象外） | 0 |

- BonDriver の約10秒ごとの記録843件と受信終了時の集計のいずれでも、上の異常は0件でした。
- 2つのパケット数の差1,535パケットは約0.13秒分で、受信停止の時点で BonDriver へ渡す前だった量に相当します。
- 平均の流量は約11,480パケット/秒（約2.16 MB/秒）で、スキャン時の各チャンネルと同じでした。
- 受信停止時の記録は `error=995`（要求による停止）、`stop_ok=1`、`fault=0` で、受信中に復帰処理は起きていません。受信を閉じた約15秒後に DriverHost はアイドル終了しました。

## 内蔵カードリーダー

- 受信中は `WinSCard.dll` を配置していました。この構成では CECH-ZD1J と PIX-DT295 の内蔵カードリーダーが列挙されます。
- DriverHost はカード操作と APDU の成否を記録しないため、TVTest がどちらのリーダーを使ったか、カード通信の回数と失敗数はこの記録に含まれません。TVTest のスクランブル数も記録していません。

## 確認していない条件

- TVTest での映像の復号と、CECH-ZD1J の内蔵カードリーダーを使い続けた状態での長時間受信
- 17ch 以外のチャンネルと、x86 版での長時間受信
- 録画ファイルへの書き出しを含む長時間録画
- USB とカードの抜き差し、無信号の継続、スリープ復帰
- 複数台の同時受信、ほかの機器と同時に受信した状態
