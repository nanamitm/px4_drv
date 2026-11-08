# PIX-DT295 WinUSB バックエンド

## 設計と検証範囲

PIX-DT295 (`06b8:103b`) の1受信機を DriverHost_PX4 が所有し、既存の BonDriver_PX4 と IPC から地デジ・BS・CSを切り替えて使用する構成です。既存の torne / PX 系の機器制御は独立したままです。

配布設定の既定は、このリポジトリのソースから生成したファームウェア（`FirmwareBackend=source-fr`）です。mAgicVISIONの常駐ファームウェアは `FirmwareBackend=magicvision` で選べます。mAgicVISIONのファイルは、torneの常駐方式とソース生成版の初期化列の参照元として同梱を続けています。`FirmwareBackend` を持たない以前の配布物のINIでは、従来どおりmAgicVISION方式で動作します。

mAgicVISION の MIT 共有コードと上流の回帰試験を `dt295/upstream` に固定版で収録し、C11 のローダー・セッション・フレーマーを C アダプター経由で利用します。上流の回帰試験にはルートの LICENSE も保持しています。出典と各ファイルの SHA-256 は `ORIGIN.json` に記載されています。WinUSB は操作全体の期限と抜去イベントを持ち、キャンセル後も OVERLAPPED の完了を回収してからメモリーを解放する契約です。

受信機を開く処理はローカル検証とバッファ確保だけで、長いUSB初期化は最初の選局へ遅延し、ReceiverManagerの一覧ロックを占有しない構成です。

選局とTS受信は別スレッドです。選局前に旧受信を回収し、ストリーム世代と端数を破棄します。常駐ファームウェアの世代番号は上流セッションが検証し、USBの制御転送は個体ごとに直列化されます。受信停止後は上流 STOP の完了を確認し、通常終了は DT295 の常駐コードを保持する上流 loader_close の契約に従います。

ファームウェア整合性、模擬転送、コマンドと状態遷移、TS分割入力、x86/x64 ビルドに加え、PIX-DT295実機で初期化・地デジ/BS/CSの選局・TS受信・再Openを確認しています。開始・終了を含むCC異常と長時間安定性には未解決の条件があり、詳細は [2026-11-06の実機検証](pix-dt295-validation.md) を参照してください。アンテナ給電と内蔵カードは未検証で、CNRはソース生成版だけが取得できます。復号には外部カードリーダーが必要です。

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
5. 既定のソース生成版は、最初の選局でRAM前像の退避と補助CPU・常駐コードの配置を行います（約2秒）。別のファームウェアがRAMに残っている場合や、ブートRAMが既知のRev.01の内容と一致しない場合は初期化が失敗します。mAgicVISION方式は正常終了後も常駐コードをRAMに残すため、方式を切り替えた直後はUSBを抜き差ししてから試します。それでも初期化に失敗する個体では `FirmwareBackend=magicvision` を指定します。ログの `PIX-DT295 source-fr` 行と `mAgicVISION` 行で、ファイル不正、Win32転送エラー、復調器のロック失敗を切り分けます。
6. 地デジ、BS/CSのTSID指定、同一方式の選局反復、地デジと衛星の切替、閉じて再オープン、無信号、USB抜去・再接続、長時間録画を個別に確認します。sync / TEI / CC、TS量と無入力時間、終了の所要時間を記録します。

USB応答不明のセッションは再送による二重実行を避けるため再利用できません。ログに物理的な再接続が必要と出た場合は、本体を接続し直してから確認します。通常終了では上流仕様に合わせ常駐コードがRAMに残ります。

無入力は5秒ごとの警告で観察し、実機の根拠がない期限で受信を終了しません。TSバッファ満杯やUSB/プロトコルの破損は受信エラーとして扱います。mAgicVISION方式ではCNRを取得できないため、信号レベル表示は実際の品質指標として利用できません。ソース生成版のCNRは後述の節を参照してください。

配布フォルダーの `channel_switch_stress.exe` は同梱BonDriverを通して選局反復とTSを検査するツールです。実行すると機器を開くため、実機接続と設定変更後に使用します。引数のspace / channelはBonDriverが列挙するインデックスで、物理チャンネル番号ではありません。

```text
channel_switch_stress.exe .\BonDriver_DT295.dll <space> <channel-a> <channel-b> <cycles> <measure-ms>
```

結果は各周期の packets / sync_errors / transport_errors / continuity_errors / 空受信区間を含みます。mAgicVISION方式のsignalはCNRを取得できないため評価対象外です。

## torne による共通経路の実機検証

既存 `CECHZD1J` 機器定義の Config に `ResidentFirmware=true` と `Experimental=true` を明示したときだけ、常駐方式を選択できます。`FirmwarePath` はDT295と共通の検証済みSBC2コンテナーです。実際のUSB IDが `054c:04b2` の場合のみtorneプロファイルを使い、衛星要求を拒否します。設定を追加しない既存torneは従来のnativeバックエンドのままです。常駐方式の内蔵カードは対象外です。

既存native経路が残したブートパッチは常駐ローダーの純正照合と一致しないため、方式の切替前にはUSBの抜き差しで初期状態へ戻す必要があります。異なるパッチを受け入れるための照合緩和はありません。常駐方式の正常終了ではtorneのブート・RAMを保存前像へ復元します。最初の書換え前には `mAgicVISION-preimage-<PID>-<tick>.bin` をホストと同じフォルダーへ保存します。

WindowsのOUT制御転送は上流のconst表を直接渡さず、完了回収まで保持する書込み可能な一時バッファを使います。torne実機で検出した `ERROR_NOACCESS (998)` の修正で、DT295にも共通する転送処理です。

torneで確認できるのはこの共通経路とtorne用プロファイルです。DT295固有のUSB発行方式、ブート配置、RF回路、BS/CS、給電、内蔵カードの実機確認は別に必要です。

## ソースからビルドするファームウェアの開発

I²Cパススルーを機器側、初期化・選局をホスト側へ置く構成の実装は [`experimental/dt295-firmware`](../experimental/dt295-firmware/README.md) にあります。ソースから生成したバンドル `dt295-source-fr.bin` は、配布物の各 `firmware` フォルダーに同梱されています。配布設定では、ソース生成版を既定の方式にしています。対象は既知のブートRAMを持つPIX-DT295 Rev.01です。

### ソース生成版を使う

`DriverHost_PX4.ini` の `[DeviceDefinition19.Config]` で `Experimental=true` を指定します。`FirmwareBackend=source-fr`、`SourceFirmwarePath`、`SourceFirmwareSha256` は配布設定に含まれています。以前の配布物のINIを使い続ける場合は、次の値を追記します。

```ini
[DeviceDefinition19.Config]
Experimental=true
FirmwarePath="firmware\magicvision-dt295.bin"
FirmwareBackend=source-fr
SourceFirmwarePath="firmware\dt295-source-fr.bin"
SourceFirmwareSha256="<同梱の値>"
```

同梱のバンドルはDTP2形式で、復調器・RF初期化列を含みません。初期化列のレジスター値は公式ファームウェア由来のテーブルと一致するため、リポジトリと配布物のバンドルには収録せず、DriverHostがバンドルの読み込み時に `FirmwarePath` のmAgicVISION 0.1.0から組み立てます。SHA-256を照合し、0.1.0以外のファイルは受け付けません。組み立てる列は、常駐コードの初期化スクリプト134レコードに、地デジ初期化末尾の3レジスターの書き直しと先頭の待機を加えたもので、実機で検証した初期化列とバイト単位で一致します。このため、ソース生成版でも `magicvision-dt295.bin` が必要です。

ソース生成版は冷間状態から起動するため、別ファームウェアがRAMに残っている場合は対象機のUSBを抜き差ししてから試します。BS/CSは外部給電環境で `LNBPower=0` を指定します。2026-11-08のWindows x64の試験では、初期化が約2.2秒、終了時の復元が約2.5秒で、`TuneTimeout` と `PipeTimeout` の既定値のまま選局と終了が完了しました。

### バンドルの再生成

同梱のバンドルはFR30向けのGNU Binutils 2.43.1とGCC 14.2.0で生成し、各ソースのSHA-256を含むmanifestを `pkg/DriverHost_PX4/firmware/dt295-source-fr.json` に記録しています。Windowsの通常ビルドにFR30のツールは不要で、`tools/check_dt295_source_firmware.py` がバンドル・manifest・現在のソースの一致を確認します。`experimental/dt295-firmware` のソースを変更するとこの確認が失敗するため、次の手順でバンドルと記録を作り直し、`DriverHost_PX4.ini` の `SourceFirmwareSha256` を出力の値へ更新します。ツールの指定方法は [README](../experimental/dt295-firmware/README.md) を参照してください。

```sh
cd winusb/experimental/dt295-firmware
python3 build.py --bulk --out build/bulk
python3 build_auxiliary.py --out build/auxiliary
python3 build_host_bundle.py build/auxiliary build/bulk ../../pkg/DriverHost_PX4/firmware/dt295-source-fr.bin \
  --runtime-init --provenance ../../pkg/DriverHost_PX4/firmware/dt295-source-fr.json
```

診断ツールや初期化列を含むDTP1を使う場合は、`extract_init_trace.py` で同梱のmAgicVISION 0.1.0からJSONLを生成します。`tests/test_init_trace.py` はPython版、`tests/dt295_frontend_init_test` はDriverHostのC++版の出力が、検証済みの列とSHA-256で一致することを確認します。

```powershell
python winusb/experimental/dt295-firmware/extract_init_trace.py winusb/pkg/DriverHost_PX4/firmware/magicvision-dt295.bin $initTrace
python winusb/experimental/dt295-firmware/build_host_bundle.py $auxiliaryBuild $bulkBuild $initTrace dt295-source-fr-dtp1.bin
```

BonDriverの選局・TS配信は既存のIPCを使います。衛星選局時には明示TSIDが必要です。`CheckLock` は選局完了時の検証結果を返します。RF状態の継続監視、内蔵B-CAS、アンテナ給電はこの経路の対象外です。CloseはTSエンジンを停止してからリング、RAM、システム設定を復元します。復元が完了しなかった場合はログに記録され、USBの電源再投入が必要です。

初期実装（コミット `4089042`、DTP1バンドル）の試験では、Windows x64の隔離環境で、同じBonDriverのOpen中にGR13 → BS01 → CS02 → GR13を各6秒受信し、保存TSのPATでTSID 31761 → 16400 → 24608 → 31761を確認しました。各保存TSの同期・TEI・CC・形式異常は0件でした。DriverHostの一部の区間では192-byte入力の再同期が1件記録されました。Close後にUSB抜き差しなしで再Openし、BS01とCS02を各4秒受信して再度復元を確認しました。初回選局は約28～29秒、以降の切り替えは約4.5～5.3秒、終了時の復元は約31～33秒でした。

### ソース生成版のCNR

ソース生成版では、BonDriverの信号レベルとして復調器TC90532XBGのC/Nを返します。補助CPU経由のI²Cで、地デジは `0x8b` から3 bytes、衛星は `0xbc` から2 bytesを読み、PX4系受信機と同じ式で0.001dB単位へ換算します。mAgicVISION方式の常駐コードは信号品質を返さないため、そちらの信号レベルは0のままです。

読み出し中はSERVICEが1回分待たされ、1回の取得に約15～30msかかります。読み出しが失敗した場合、受信は継続し、次の選局まで信号レベルの取得だけを停止します。

PIX-DT295 Rev.01とWindows x64で、500msごとの取得を加えた各30秒の受信と、CS ND02の250msごと・10分間の受信で、同期・TEI・CC異常は0件、受信量の低下もありませんでした。同じPCに接続したPX-W3U4（TC90522）と比べると、BS01/TS0は19.06 / 18.98dB、CS ND02は16.69 / 16.70dBでした。地デジ13chは31.28 / 28.22dBで、3dBの差が機器の入力系統によるものか、換算の差によるものかは未確認です。

CNRを250msごとに取得する10分試験の1回目では、試験中にPX-W3U4を接続した約2分後、CN読み出しの制御転送が約1秒応答せずに受信が停止し、Closeの復元も失敗しました。USB接続を変えずに同じ条件で再試験した2回目は完走しています。原因は特定していないため、受信中にほかのUSB機器を抜き差しする条件は未確認として扱います。

## 比較試験用の主CPU USB直接制御

`FirmwareBackend=direct-fr` はPIX-DT295 Rev.01向けの実験経路です。BonDriver IPC、選局ロジック、TS配信バッファは既存のDriverHostと共有します。機器側は主CPUのboot0に一時的なPIF1フックを置き、I²CとTS送出をそこで実行します。初期化列は従来の `source-fr` と同じ参照ファイルから構成し、初期化の連続書き込みは最大40バイトの操作列にまとめます。主CPUが192バイトのリングレコードを188バイトTSに整形し、HostはBulk INに8 KiBの読み取りを64本投入します。C/Nは同じ復調レジスタとdB換算式を使用します。

FR30 GCCとBinutilsを用意し、リポジトリルートで以下を実行すると比較用のDTP3バンドルができます。

```powershell
python winusb/experimental/dt295-firmware/build.py --direct-usb --out build/dt295-direct
python winusb/experimental/dt295-firmware/build_direct_bundle.py build/dt295-direct build/dt295-direct-fr.bin
```

隔離したDriverHost環境にバンドルをコピーし、既存のPIX-DT295定義で以下を指定します。SHA-256はバンドル生成時の出力を使います。`FirmwarePath` は既存のmAgicVISION 0.1.0参照ファイルを指します。これは初期化データの抽出に使われます。

```ini
[DeviceDefinition19.Config]
Experimental=true
FirmwareBackend=direct-fr
SourceFirmwarePath="firmware\dt295-direct-fr.bin"
SourceFirmwareSha256="<バンドル生成時に表示されるSHA-256>"
FirmwarePath="firmware\magicvision-dt295.bin"
```

同じ実機で切り替える場合は先に受信を閉じ、復元結果を確認してから次のバックエンドを起動します。比較条件として放送・C/N取得頻度・時間を揃えると、停止とCC異常を別々に比較できます。復元に失敗した場合はログに残り、再試験には対象機のUSB電源再投入が必要です。内蔵B-CASとアンテナ給電は今回の比較範囲外です。実機での初期化、選局、連続受信、復元は未検証です。
