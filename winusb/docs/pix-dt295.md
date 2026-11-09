# PIX-DT295 WinUSB バックエンド

## 設計と検証範囲

PIX-DT295 (`06b8:103b`) の1受信機を DriverHost_PX4 が所有し、既存の BonDriver_PX4 と IPC から地デジ・BS・CSを切り替えて使用する構成です。既存の torne / PX 系の機器制御は独立したままです。

PIX-DT295の既定は `direct-fr` です。`FirmwareBackend=source-fr` では従来の192バイトTS転送を使います。両方式とも主CPUでI²Cを処理します。両方式のファームウェアをDriverHostに組み込んでいます。旧INIの `FirmwareBackend=magicvision` は行を削除するか `direct-fr` に変更します。旧配布INIに記載された各方式の既定のパスとSHA-256の組も、組み込み版に移行します。独自バンドルを指定していたINIでは、`SourceFirmwarePath` と `SourceFirmwareSha256` を削除すると、選択した方式の組み込み版で起動します。

mAgicVISION の MIT 共有コードと上流の回帰試験を `dt295/upstream` に固定版で収録し、C11 のローダー・セッション・フレーマーを C アダプター経由で利用します。上流の回帰試験にはルートの LICENSE も保持しています。出典と各ファイルの SHA-256 は `ORIGIN.json` に記載されています。WinUSB は操作全体の期限と抜去イベントを持ち、キャンセル後も OVERLAPPED の完了を回収してからメモリーを解放する契約です。

受信機を開く処理はローカル検証とバッファ確保だけで、長いUSB初期化は最初の選局へ遅延し、ReceiverManagerの一覧ロックを占有しない構成です。

選局とTS受信は別スレッドです。選局前に旧受信を回収し、ストリーム世代と端数を破棄します。USB制御転送は個体ごとに直列化し、通常終了時にはTSエンジンの停止とRAMの復元を確認します。

ファームウェア整合性、模擬転送、コマンドと状態遷移、TS分割入力、x86/x64 ビルドに加え、PIX-DT295実機で初期化・地デジ/BS/CSの選局・TS受信・再Openを確認しています。開始・終了を含むCC異常と長時間安定性には未解決の条件があり、詳細は [2026-11-06の実機検証](pix-dt295-validation.md) を参照してください。アンテナ給電と内蔵カードは未検証で、CNRはソース生成版だけが取得できます。復号には外部カードリーダーが必要です。

## 旧常駐方式

mAgicVISIONの実行経路は配布版から廃止しました。旧ファームウェアはリポジトリのオフライン回帰試験と過去の計測の再現用です。送出上限256回版の実機結果は [検証記録](pix-dt295-256-validation.md) にあります。

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
3. `DriverHost_PX4.ini` の `[DeviceDefinition20.Config]` を `Experimental=true` へ変更します。初期設定はfalseのため受信機を登録せず、ファームウェアの転送もありません。
4. 外部カードリーダーと外部アンテナ給電を用意します。BonDriverの `LNBPower=0` を維持し、未実装の内蔵カードや給電制御を前提にしない構成で確認します。
5. ソース生成版は初回選局でRAM前像を保存し、一時コードを配置します（約1.3～2秒）。旧ファームウェアがRAMに残っている場合はUSBを抜き差しします。既知のRev.01とブートRAMが一致しない機器では初期化に失敗します。ログの `PIX-DT295 direct-fr` と `PIX-DT295 source-fr` の行で初期化結果を確認できます。
6. 地デジ、BS/CSのTSID指定、同一方式の選局反復、地デジと衛星の切替、閉じて再オープン、無信号、USB抜去・再接続、長時間録画を個別に確認します。sync / TEI / CC、TS量と無入力時間、終了の所要時間を記録します。

USB応答不明のセッションは再送による二重実行を避けるため再利用できません。ログに物理的な再接続が必要と出た場合は、本体を接続し直してから確認します。正常終了では保存したRAMと設定を復元します。

無入力は5秒ごとの警告で観察し、実機の根拠がない期限で受信を終了しません。TSバッファ満杯やUSB/プロトコルの破損は受信エラーとして扱います。ソース生成版のCNRは後述の節を参照してください。

配布フォルダーの `channel_switch_stress.exe` は同梱BonDriverを通して選局反復とTSを検査するツールです。実行すると機器を開くため、実機接続と設定変更後に使用します。引数のspace / channelはBonDriverが列挙するインデックスで、物理チャンネル番号ではありません。

```text
channel_switch_stress.exe .\BonDriver_DT295.dll <space> <channel-a> <channel-b> <cycles> <measure-ms>
```

結果は各周期の packets / sync_errors / transport_errors / continuity_errors / 空受信区間を含みます。

## torneの旧設定

`ResidentFirmware=true` が残っている場合は警告を出し、ネイティブ経路で起動します。旧 `I2cBackend=source-fr` は `native` に変更するか、その行を削除します。旧常駐方式専用のINIを使っていた場合は通常のtorne用INIへ移行します。過去の測定結果は [torne常駐方式の実機記録](torne-resident-validation.md) にあります。

## ソースからビルドするファームウェアの開発

I²Cパススルーを機器側、初期化・選局をホスト側へ置く構成の実装は [`experimental/dt295-firmware`](../experimental/dt295-firmware/README.md) にあります。188バイト転送のDTP3と192バイト転送のDTP4は、どちらもDriverHostに組み込みます。配布設定の既定はDTP3です。対象は既知のブートRAMを持つPIX-DT295 Rev.01です。

### 192バイト転送版を使う

`DriverHost_PX4.ini` の `[DeviceDefinition20.Config]` を次のように設定します。

```ini
[DeviceDefinition20.Config]
Experimental=true
FirmwareBackend=source-fr
```

組み込みのDTP4（192バイト転送）とDTP3（188バイト転送）は、DriverHostの [組み込み初期化列](../src/DriverHost_PX4/dt295/frontend_init.cpp) を共用します。実機検証済みDTP1の初期化セクションと同じ754バイトで、地デジと衛星の書き込み、マスク更新、待機の順序を含みます。ソース生成版を動かすための外部初期化ファイルやmAgicVISIONバイナリは不要です。

旧 `source-fr`（DTP2）は補助CPUでI²Cを処理していました。DTP4では主CPUへ移し、192バイトのリングレコード、PIF1のTS転送、ホストの16 KiB × 4本のBulk IN要求を維持しています。旧版と比較する場合は、ファームウェアの切り替え前にUSBを抜き差しします。

### バンドルの再生成

組み込みのDTP4バンドルはFR30向けのGNU Binutils 2.43.1とGCC 14.2.0で生成し、各ソースのSHA-256を含むmanifestを `pkg/DriverHost_PX4/firmware/dt295-source-fr.json` に記録しています。Windowsの通常ビルドにFR30のツールは不要で、`tools/check_dt295_source_firmware.py` がバンドル・manifest・現在のソースの一致を確認します。`experimental/dt295-firmware` のソースを変更するとこの確認が失敗するため、次の手順でバンドルと記録を作り直してDriverHostを再ビルドします。ツールの指定方法は [README](../experimental/dt295-firmware/README.md) を参照してください。

```sh
cd winusb/experimental/dt295-firmware
python3 build.py --direct-usb --raw-ts --out build/source-fr
python3 build_direct_bundle.py build/source-fr ../../pkg/DriverHost_PX4/firmware/dt295-source-fr.bin \
  --provenance ../../pkg/DriverHost_PX4/firmware/dt295-source-fr.json
```

診断ツール用の初期化列や過去の試験用DTP1を再生成する場合は、`extract_init_trace.py` で同梱のmAgicVISION 0.1.0からJSONLを生成します。`tests/test_init_trace.py` はPython版、`tests/dt295_frontend_init_test` はDriverHostの組み込み列が、検証済みの列とSHA-256で一致することを確認します。

```powershell
python winusb/experimental/dt295-firmware/extract_init_trace.py winusb/pkg/DriverHost_PX4/firmware/magicvision-dt295.bin $initTrace
python winusb/experimental/dt295-firmware/build_host_bundle.py $auxiliaryBuild $bulkBuild $initTrace dt295-source-fr-dtp1.bin
```

BonDriverの選局・TS配信は既存のIPCを使います。衛星選局時には明示TSIDが必要です。`CheckLock` は選局完了時の検証結果を返します。RF状態の継続監視、内蔵B-CAS、アンテナ給電はこの経路の対象外です。CloseはTSエンジンを停止してからリング、RAM、システム設定を復元します。復元が完了しなかった場合はログに記録され、USBの電源再投入が必要です。

初期実装（コミット `4089042`、DTP1バンドル）の試験では、Windows x64の隔離環境で、同じBonDriverのOpen中にGR13 → BS01 → CS02 → GR13を各6秒受信し、保存TSのPATでTSID 31761 → 16400 → 24608 → 31761を確認しました。各保存TSの同期・TEI・CC・形式異常は0件でした。DriverHostの一部の区間では192-byte入力の再同期が1件記録されました。Close後にUSB抜き差しなしで再Openし、BS01とCS02を各4秒受信して再度復元を確認しました。初回選局は約28～29秒、以降の切り替えは約4.5～5.3秒、終了時の復元は約31～33秒でした。

### ソース生成版のCNR

ソース生成版では、BonDriverの信号レベルとして復調器TC90532XBGのC/Nを返します。I²Cで、地デジは `0x8b` から3 bytes、衛星は `0xbc` から2 bytesを読み、PX4系受信機と同じ式で0.001dB単位へ換算します。

読み出し中はSERVICEが1回分待たされ、1回の取得に約15～30msかかります。読み出しが失敗した場合、受信は継続し、次の選局まで信号レベルの取得だけを停止します。

PIX-DT295 Rev.01とWindows x64で、500msごとの取得を加えた各30秒の受信と、CS ND02の250msごと・10分間の受信で、同期・TEI・CC異常は0件、受信量の低下もありませんでした。同じPCに接続したPX-W3U4（TC90522）と比べると、BS01/TS0は19.06 / 18.98dB、CS ND02は16.69 / 16.70dBでした。地デジ13chは31.28 / 28.22dBで、3dBの差が機器の入力系統によるものか、換算の差によるものかは未確認です。

CNRを250msごとに取得する10分試験の1回目では、試験中にPX-W3U4を接続した約2分後、CN読み出しの制御転送が約1秒応答せずに受信が停止し、Closeの復元も失敗しました。USB接続を変えずに同じ条件で再試験した2回目は完走しています。原因は特定していないため、受信中にほかのUSB機器を抜き差しする条件は未確認として扱います。

その後、補助CPUでI²Cを処理していた旧 `source-fr`（DTP2）の CS ND06 の60分試験（250msごとの取得）では、ほかのUSB機器を操作しない条件でも、26.8分でCN読み出しの制御転送が約1秒応答せずに受信が停止しました。USB応答不明の転送後はセッションを再利用しない契約のため、続く選局も失敗し、復元にはUSBの抜き差しが必要でした。CNRを取得しない60分試験と、CN読み出しと同じ回数だけ補助CPUの共有RAMを読み書きし、I²Cは起動しない60分試験は、どちらも停止しませんでした。補助CPUがI²Cを実行している間に主CPUの制御転送が応答しなくなる条件が疑われますが、各条件1回の試験で、仕組みは特定していません。この結果から、配布設定の既定は補助CPUを使わない主CPU版へ移しました。現在の `source-fr`（DTP4）も主CPUでI²Cを処理するため、この条件の対象外です。DTP4の長時間試験は行っていません。

## 主CPU USB直接制御（既定）

`FirmwareBackend=direct-fr` はPIX-DT295 Rev.01向けの経路で、配布設定の既定です。補助CPUを起動しません。BonDriver IPC、選局ロジック、TS配信バッファは既存のDriverHostと共有します。機器側は主CPUのboot0に一時的なPIF1フックを置き、I²CとTS送出をそこで実行します。初期化列は `source-fr` と共用し、初期化の連続書き込みは最大40バイトの操作列にまとめます。主CPUが192バイトのリングレコードを188バイトTSに整形し、HostはBulk INに8 KiBの読み取りを64本投入します。C/Nは同じ復調レジスタとdB換算式を使用します。

配布設定には次の値が含まれています。以前の配布物のINIを使い続ける場合は、次の値を追記します。

```ini
[DeviceDefinition20.Config]
Experimental=true
FirmwareBackend=direct-fr
```

組み込みDTP3の各ソースのSHA-256は `pkg/DriverHost_PX4/firmware/dt295-direct-fr.json` に記録され、`tools/check_dt295_source_firmware.py` がDTP4と同じくバンドル・manifest・現在のソースの一致を確認します。ソースを変更した場合は、FR30 GCCとBinutilsを用意し、リポジトリルートで次を実行してバンドルと記録を作り直し、DriverHostを再ビルドします。バンドルはビルド入力としてリポジトリに置かれ、実行時にはEXE内のリソースから読み出します。

```powershell
python winusb/experimental/dt295-firmware/build.py --direct-usb --out build/dt295-direct
python winusb/experimental/dt295-firmware/build_direct_bundle.py build/dt295-direct winusb/pkg/DriverHost_PX4/firmware/dt295-direct-fr.bin `
  --provenance winusb/pkg/DriverHost_PX4/firmware/dt295-direct-fr.json
```

方式を切り替える場合は先に受信を閉じ、復元結果を確認してから次の方式を起動します。復元に失敗した場合はログに残り、再試験には対象機のUSB電源再投入が必要です。

PIX-DT295 Rev.01とWindows x64で、CNRを250msごとに取得する条件の実機試験を行いました。BonDriver出力の同期・TEI・CC異常は全試験で0件、終了時の復元も全試験で確認しています。

| 放送 | 時間 | パケット数 | C/N | CN取得1回の最大 |
|---|---|---|---|---|
| 地デジ13ch | 30秒 | 596,992 | 30.6～31.8dB | 50.7ms |
| BS01/TS0 | 30秒 | 346,112 | 18.8～19.0dB | 25.5ms |
| CS ND06 | 15分 | 23,412,736 | 16.7～17.0dB | 55.1ms |
| CS ND06 | 60分（2回） | 各93,656,064 | 16.6～17.0dB | 54.7ms / 34.2ms |

初期化は約1.3秒、終了時の復元は約2.5秒でした。地デジ13chの試験ではDriverHostの受信段でTEIが2件記録され、BonDriver出力では0件でした。これらの試験では、ファームウェアを外部ファイルから読み込む版のDriverHostを使いました。読み込んだDTP3は、現在DriverHostに組み込んでいるものとバイト単位で一致します。

ファームウェアを組み込んだDriverHost（コミット `e868166`）では、TVTestで次の点を確認しました。

- 視聴とチャンネルスキャンに問題はありませんでした。
- BSの長時間視聴でも問題はありませんでした。DriverHostのログが上書きされたため、チャンネル・時間・TSの計数は記録が残っていません。
- 地デジ13chを6時間20分連続で受信し（約4.54億パケット）、受信の停止、TEI、同期異常、データの途切れはありませんでした。DriverHostのログにエラーと警告はありません。BonDriver出力ではCC異常が4件記録されました。4件とも12:00、13:10、14:30、15:00の各2秒後に、PID 0x0111だけで起きています。放送局側の番組切り替えによるものと推定していますが、同じ放送を別の受信機で同時に受信して比べる確認は行っていません。

チャンネル切り替えの反復（`channel_switch_stress`）、受信中のUSB抜去、内蔵B-CAS、アンテナ給電は未検証です。
