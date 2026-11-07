# PIX-DT295 WinUSB バックエンド

## 設計と検証範囲

PIX-DT295 (`06b8:103b`) の1受信機を DriverHost_PX4 が所有し、既存の BonDriver_PX4 と IPC から地デジ・BS・CSを切り替えて使用する構成です。既存の torne / PX 系の機器制御は独立したままです。

mAgicVISION の MIT 共有コードと上流の回帰試験を `dt295/upstream` に固定版で収録し、C11 のローダー・セッション・フレーマーを C アダプター経由で利用します。上流の回帰試験にはルートの LICENSE も保持しています。出典と各ファイルの SHA-256 は `ORIGIN.json` に記載されています。WinUSB は操作全体の期限と抜去イベントを持ち、キャンセル後も OVERLAPPED の完了を回収してからメモリーを解放する契約です。

受信機を開く処理はローカル検証とバッファ確保だけで、長いUSB初期化は最初の選局へ遅延し、ReceiverManagerの一覧ロックを占有しない構成です。

選局とTS受信は別スレッドです。選局前に旧受信を回収し、ストリーム世代と端数を破棄します。常駐ファームウェアの世代番号は上流セッションが検証し、USBの制御転送は個体ごとに直列化されます。受信停止後は上流 STOP の完了を確認し、通常終了は DT295 の常駐コードを保持する上流 loader_close の契約に従います。

ファームウェア整合性、模擬転送、コマンドと状態遷移、TS分割入力、x86/x64 ビルドに加え、PIX-DT295実機で初期化・地デジ/BS/CSの選局・TS受信・再Openを確認しています。開始・終了を含むCC異常と長時間安定性には未解決の条件があり、詳細は [2026-11-06の実機検証](pix-dt295-validation.md) を参照してください。アンテナ給電、内蔵カードと CNR は未検証で、復号には外部カードリーダーが必要です。

## ファームウェア

- Release: https://github.com/makeding/mAgicVISION/releases/tag/v1
- Package: `magicvision-firmware_0.1.0+20261104.160644_all.deb`
- Package SHA-256: `c5ad07da6d384282ac1f3e8bc115b39c5e211a5029884c3466a26979174a35f7`
- `firmware.bin` SHA-256: `d9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f`
- Size: 289424 bytes; SBC2 v1; resident ABI 2; build ID `bd7efbdd5060f36c`

上流 README はファームウェア成果物の再配布を認めています。収録ファイル名は `firmware/magicvision-dt295.bin` です。対応する常駐コードのソースは上流公開ツリーに含まれていません。

## 送出上限256回のファームウェア

CSや地デジの定常受信中にリングが詰まる条件では、送出上限を256回に増やしたPIX-DT295用イメージを `FirmwarePath` で選べます。生成器はmAgicVISION 0.1.1の `magicvision-dt295.bin` のSHA-256を検証し、計測版で実機確認した送出ループを組み込みます。生成に必要なのはPython 3です。

リポジトリルートから、取得した0.1.1のイメージを指定します。

```powershell
python winusb/tools/build_dt295_stream256.py path/to/magicvision-dt295.bin magicvision-dt295-stream256.bin
```

生成ファイルを `DriverHost_PX4.exe` のあるフォルダーの `firmware` サブフォルダーに置き、PIX-DT295の機器定義を次のように設定します。

```ini
[DeviceDefinition19.Config]
Experimental=true
FirmwarePath="firmware\magicvision-dt295-stream256.bin"
```

切り替え時は対象機器の受信を終了し、旧イメージがRAMに残っている場合はPIX-DT295のUSBを抜き差ししてから、新しい設定でHostを起動します。元のイメージに戻す場合も同じ手順で `FirmwarePath` を変更します。

生成したイメージでCS ND02、地デジ13ch、BS01/TS0を各30秒測定し、全保存TSでCC・同期・TEI異常が0件でした。CSのHost入力では開始後373.131 msまでにCC関連異常25件があり、その後は増えませんでした。模擬USB試験はx86/x64、実機試験はWindows x64で実施しました。送出上限の比較と、計測版での5分連続受信・繰り返し選局の結果も参照できます。[上限の比較](pix-dt295-budget-comparison.md) [連続受信と選局試験](pix-dt295-256-validation.md)

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

## ソースからビルドするファームウェアの開発

I²Cパススルーを機器側、初期化・選局をホスト側へ置く構成の実験実装は [`experimental/dt295-firmware`](../experimental/dt295-firmware/README.md) にあります。現段階は独立したFRプロトコルコアと診断用ブートアダプターのビルド・命令モデル検証で、ロード/復元、実機でのUSB接続、物理I²C、TSの実装・検証は残っています。通常の配布物や機器定義には接続されていないため、本ページのSBC2方式と置き換えて使える段階ではありません。

### ソース生成版をBonDriverで試す

対象は既知のブートRAMを持つPIX-DT295 Rev.01です。`build.py --bulk` と `build_auxiliary.py` の出力、および衛星初期化を含む地デジ・衛星JSONLから、DriverHost用のバンドルを生成します。生成器が表示するSHA-256を設定に使います。

```powershell
python winusb/experimental/dt295-firmware/build_host_bundle.py $auxiliaryBuild $bulkBuild $initTrace dt295-source-fr.bin
```

バンドルをDriverHostの `firmware` フォルダーに置き、既存のPIX-DT295定義を次のように設定します。`SourceFirmwareSha256` は生成器の出力に置き換えます。

```ini
[DeviceDefinition19.Config]
Experimental=true
FirmwareBackend=source-fr
SourceFirmwarePath="firmware\dt295-source-fr.bin"
SourceFirmwareSha256="<64桁のSHA-256>"
```

`BonDriver_DT295.ini` の `[BonDriver]` に `TuneTimeout=150000` を指定し、試験用DriverHostの `[DriverHost_PX4]` には `PipeTimeout=240000` を指定します。初回選局にはRAM前像の退避、Closeには復元が含まれます。ソース生成版は冷間状態から起動するため、別ファームウェアがRAMに残っている場合は対象機のUSBを抜き差ししてから試します。BS/CSは外部給電環境で `LNBPower=0` を指定します。

BonDriverの選局・TS配信は既存のIPCを使います。衛星選局時には明示TSIDが必要です。`CheckLock` は選局完了時の検証結果を返します。RF状態の継続監視、CNR、内蔵B-CAS、アンテナ給電はこの経路の対象外です。CloseはTSエンジンを停止してからリング、RAM、システム設定を復元します。復元が完了しなかった場合はログに記録され、USBの電源再投入が必要です。

Windows x64の隔離環境では、同じBonDriverのOpen中にGR13 → BS01 → CS02 → GR13を各6秒受信し、保存TSのPATでTSID 31761 → 16400 → 24608 → 31761を確認しました。各保存TSの同期・TEI・CC・形式異常は0件でした。DriverHostの一部の区間では192-byte入力の再同期が1件記録されました。Close後にUSB抜き差しなしで再Openし、BS01とCS02を各4秒受信して再度復元を確認しました。初回選局は約28～29秒、以降の切り替えは約4.5～5.3秒、終了時の復元は約31～33秒でした。
