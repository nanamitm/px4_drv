# PIX-DT295 WinUSB バックエンド

## 設計と検証範囲

PIX-DT295 (`06b8:103b`) の1受信機を DriverHost_PX4 が所有し、既存の BonDriver_PX4 と IPC から地デジ・BS・CSを切り替えて使用する構成です。既存の torne / PX 系の機器制御は独立したままです。

mAgicVISION の MIT 共有コードと上流の回帰試験を `dt295/upstream` に固定版で収録し、C11 のローダー・セッション・フレーマーを C アダプター経由で利用します。上流の回帰試験にはルートの LICENSE も保持しています。出典と各ファイルの SHA-256 は `ORIGIN.json` に記載されています。WinUSB は操作全体の期限と抜去イベントを持ち、キャンセル後も OVERLAPPED の完了を回収してからメモリーを解放する契約です。

受信機を開く処理はローカル検証とバッファ確保だけで、長いUSB初期化は最初の選局へ遅延し、ReceiverManagerの一覧ロックを占有しない構成です。

選局とTS受信は別スレッドです。選局前に旧受信を回収し、ストリーム世代と端数を破棄します。常駐ファームウェアの世代番号は上流セッションが検証し、USBの制御転送は個体ごとに直列化されます。受信停止後は上流 STOP の完了を確認し、通常終了は DT295 の常駐コードを保持する上流 loader_close の契約に従います。

実機到着前の対象はファームウェア整合性、模擬転送、コマンドと状態遷移、TS分割入力、x86/x64 ビルドです。受信の実績、アンテナ給電、長時間安定性は未検証です。内蔵カードと CNR の公開は検証対象を受信へ絞るため対象外で、外部カードリーダーが必要です。

## ファームウェア

- Release: https://github.com/makeding/mAgicVISION/releases/tag/v1
- Package: `magicvision-firmware_0.1.0+20261104.160644_all.deb`
- Package SHA-256: `c5ad07da6d384282ac1f3e8bc115b39c5e211a5029884c3466a26979174a35f7`
- `firmware.bin` SHA-256: `d9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f`
- Size: 289424 bytes; SBC2 v1; resident ABI 2; build ID `bd7efbdd5060f36c`

上流 README はファームウェア成果物の再配布を認めています。収録ファイル名は `firmware/magicvision-dt295.bin` です。対応する常駐コードのソースは上流公開ツリーに含まれていません。

## ビルドとオフライン試験

Visual Studio 2022 の v143 と Windows SDK を用意し、リポジトリルートで次を実行します。

```powershell
python winusb/tools/run_dt295_tests.py
```

x86/x64 の上流 bootstrap / loader / session / USB window / stream input、Windows制御転送のキャンセル・短い応答・抜去・失敗後の再送禁止、実ファームウェアを使う模擬USBの初期化・地デジ・衛星・TS受信・世代不一致・終了・再オープンを検証します。模擬USBはRFチップや常駐コードの実行を再現するものではありません。

通常の `winusb/build.ps1` はこの試験を含み、既存DLLから `BonDriver_DT295.dll` を作る配布フォルダーへ設定・チャンネル表・ファームウェアを配置します。署名済みINF/CATは `pkg/inf` と共通のドライバーパッケージです。

## 実機到着後

1. `06b8:103b` のPIX-DT295であることを確認し、`PIX-DT295.inf` を使用する機器だけへWinUSBを割り当てます。PIX-DT295 Wなど別PIDは対象外です。
2. `BonDriver_DT295_32bit` または `64bit` をアプリのビット数に合わせて配置します。ファームウェアは実際に起動するDriverHostと同じディレクトリの `firmware` 以下に必要です。
3. `DriverHost_PX4.ini` の `[DeviceDefinition19.Config]` を `Experimental=true` へ変更します。初期設定はfalseのため受信機を登録せず、ファームウェアの転送もありません。
4. 外部カードリーダーと外部アンテナ給電を用意します。BonDriverの `LNBPower=0` を維持し、未実装の内蔵カードや給電制御を前提にしない構成で確認します。
5. 最初の選局にはRAMの読戻しとファームウェア配置を伴うため時間がかかります。ログの `mAgicVISION` 行でファイル不正、Win32転送エラー、常駐応答エラーを切り分けます。
6. 地デジ、BS/CSのTSID指定、同一方式の選局反復、地デジと衛星の切替、閉じて再オープン、無信号、USB抜去・再接続、長時間録画を個別に確認します。sync / TEI / CC、TS量と無入力時間、終了の所要時間を記録します。

USB応答不明のセッションは再送による二重実行を避けるため再利用できません。ログに物理的な再接続が必要と出た場合は、本体を接続し直してから確認します。通常終了では上流仕様に合わせ常駐コードがRAMに残ります。

無入力は5秒ごとの警告で観察し、実機の根拠がない期限で受信を終了しません。TSバッファ満杯やUSB/プロトコルの破損は受信エラーとして扱います。CNR未実装のため信号レベル表示は実際の品質指標として利用できません。

配布フォルダーの `channel_switch_stress.exe` は同梱BonDriverを通して選局反復とTSを検査するツールです。実行すると機器を開くため、実機接続と設定変更後に使用します。引数のspace / channelはBonDriverが列挙するインデックスで、物理チャンネル番号ではありません。

```text
channel_switch_stress.exe .\BonDriver_DT295.dll <space> <channel-a> <channel-b> <cycles> <measure-ms>
```

結果は各周期の packets / sync_errors / transport_errors / continuity_errors / 空受信区間を含みます。signalはCNR未実装のため評価対象外です。

## torne による共通経路の実機検証

既存 `CECHZD1J` 機器定義の Config に `ResidentFirmware=true` と `Experimental=true` を明示したときだけ、常駐方式を選択できます。`FirmwarePath` はDT295と共通の検証済みSBC2コンテナーです。実際のUSB IDが `054c:04b2` の場合のみtorneプロファイルを使い、衛星要求を拒否します。設定を追加しない既存torneは従来のnativeバックエンドのままです。常駐方式の内蔵カードは対象外です。

既存native経路が残したブートパッチは常駐ローダーの純正照合と一致しないため、方式の切替前にはUSBの抜き差しで初期状態へ戻す必要があります。異なるパッチを受け入れるための照合緩和はありません。常駐方式の正常終了ではtorneのブート・RAMを保存前像へ復元します。最初の書換え前には `mAgicVISION-preimage-<PID>-<tick>.bin` をホストと同じフォルダーへ保存します。

WindowsのOUT制御転送は上流のconst表を直接渡さず、完了回収まで保持する書込み可能な一時バッファを使います。torne実機で検出した `ERROR_NOACCESS (998)` の修正で、DT295にも共通する転送処理です。

torneで確認できるのはこの共通経路とtorne用プロファイルです。DT295固有のUSB発行方式、ブート配置、RF回路、BS/CS、給電、内蔵カードの実機確認は別に必要です。
