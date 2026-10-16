> [!NOTE]
> このドキュメントは [n0atl/px4_drv](https://github.com/n0atl/px4_drv) (feature/tsmf-tlv-v61) の README を、統合にあたって独立したドキュメントとして収録したものです。

# ISDB-C (トランスモジュレーション) 対応 (実験的)

PX-MLT 系チューナーに、CATV のトランスモジュレーション方式（ISDB-C / ITU-T J.83 Annex C、以下「トラモジ」）の受信機能を追加した実験版です。

ベースは [tsukumijima/px4_drv](https://github.com/tsukumijima/px4_drv) です。本リポジトリは PLEX / e-Better の公式ドライバではなく、公式ドライバと併用するものでもありません。

> [!WARNING]
> この版の ISDB-C 復調初期化は、公開されているCXD2856ER向けLinuxドライバを参考にした実験的な実装です。CATV環境で受信を確認していますが、J.83Cへの規格適合性を検証したものではなく、すべてのCATV局・周波数・機種での動作を保証しません。
Linux版のISDB-C機能は実機未検証です。通常のISDB-C TS受信経路のみを実験的に含めており、WinUSB版のTSMF Relative stream抽出およびTLV生出力は未対応です。

## 対応範囲

| 機種 | チップ構成 | ISDB-C の状況 |
| --- | --- | --- |
| PX-MLT5PE | CXD2856ER / CXD2858ER ×5 | 実機で受信確認済み |
| DTV02A-5TS-P | PX-MLT5PE互換 ×5 | ソース上で対応、実機未確認 |
| PX-MLT8PE | 同一筐体内に3チューナー側と5チューナー側 | ソース上で対応、実機未確認 |
| DTV02A-4TS-P | CXD2856ER / CXD2858ER ×4 | 実機で受信確認済み |

上記以外のチューナーは、このトラモジ機能の対象外です。

## 実装している受信条件

- 変調方式: 64QAM / 256QAM（WinUSB版・特定環境で受信確認
- シンボルレート: 5274 ksym/s
- 帯域幅: 6 MHz
- CATV C13～C63 と、UHF帯のトラモジ再送信
- TSMF: Relative stream 番号を指定したTSMFストリームの抽出

シンボルレートと帯域幅は現時点では固定です。WinUSB版は既定で64QAMを先に試し、ロックしなかった場合に256QAM / 5274 ksym/sで再試行します。ただし64QAMとして誤ロックする256QAMチャンネルがあるため、ChSetでチャンネルごとに64または256QAMを明示指定できます。256QAMの動作は公開ドライバとの比較に基づく実験的なものです。

## Windows (WinUSB)

`BonDriver_PX-MLT.dll` ひとつで地デジ・BS/CS・トラモジを切り替えられます。同じDLLを複数のTVTestやEDCBプロセスから開いた場合、空いているチューナーが割り当てられます。

例: PX-MLT5PEでは「地デジ3番組 + トラモジ2番組」、PX-MLT8PEでは合計8番組の同時利用が可能です。実際に同時受信できる番組数は、アンテナ配線、USB帯域、PC性能、信号品質にも依存します。

### 配置

ビルド後に生成される `BonDriver_PX-MLT_64bit` または `BonDriver_PX-MLT_32bit` フォルダの内容を、TVTest / EDCB が使用するBonDriverフォルダへ**すべて**コピーしてください。

最低限、次のファイルは同じフォルダに必要です。

```text
BonDriver_PX-MLT.dll
BonDriver_PX-MLT.ini
BonDriver_PX4-C.ChSet.txt
BonDriver_PX4-S.ChSet.txt
BonDriver_PX4-T.ChSet.txt
DriverHost_PX4.exe
DriverHost_PX4.ini
it930x-firmware.bin
```

スクランブル解除に内蔵カードリーダーを使う場合は、同梱の `WinSCard.dll` をTVTestやEDCBの実行ファイルと同じフォルダへ配置してください。DLLのビット数はアプリケーションに合わせます。

WinUSBドライバのインストール方法、自己署名証明書、B-CASカードリーダーの詳細は、[README](../README.md) を参照してください。

### トラモジChSet

トラモジのChSetは `BonDriver_PX4-C.ChSet.txt` です。

各行の項目は次のとおりです。

```text
名前 / 空間番号 / チャンネル番号 / PX-MLT物理チャンネル番号 / TSID /
任意のRF中心周波数[kHz] / 任意の周波数補正[kHz] /
任意のTSMF Relative stream番号（0～14）または変調方式（64 / 256）または通常TS用変調方式（64 / 256） /
任意のTSMF slot offset / 任意のTSMF用変調方式（64 / 256） / 任意のTSMF出力形式（1=生TLV）
```

- CATV Cチャンネルでは、6番目を `0` にすると物理チャンネル番号から中心周波数を計算します。
- UHF・VHF帯はCチャンネルと番号体系が異なるため、6番目に中心周波数を指定します。
- 7番目は受信周波数の補正値です。放送側の設定ではありません。
- 8番目に`0`～`14`を指定した行はTSMFとして扱います。`0` はRelative stream 0を意味します。
- 8番目に`64`または`256`を指定すると、通常チャンネルの変調方式を固定します。例: `15Ch    1    2    65    0    485000    -625    256`。
- TSMFチャンネルにも変調方式を固定する場合は、9番目にTSMF slot offset、10番目に`64`または`256`を指定します。
- 11番目に`1`を指定すると、TSMF内のTLV型ストリーム（主にBS4Kなど）を生TLVで出力します。通常のチャンネルでは使用しないでください。これは `BonDriver_dantto4k.dll` の内側BonDriver用の実験機能であり、 `BonDriver_PX-MLT.dll` をTVTest / EDCBから直接開く通常用途では指定しません。

周波数補正は、映る範囲の中央を目安に25 kHz刻みで設定してください。たとえば `-550` が中心付近なら、ロックしないときは `-525`、`-575` のように調整します。補正値は受信環境（CATV局）ごとに異なる可能性があります。


### nullパケット除去

PID 0x1FFF のnullパケットを除去します。TSMFにも対応しています。


```ini
DiscardNullPackets=true
```

`false` にすると、TSMF・通常チャンネルのどちらでもnullパケットをそのまま出力します。

### TSMFヘッダ PID `0x002F` の除去

`0x002F` はTSMFの多重フレームヘッダです。`DiscardTsmfHeaderPackets=true` にすると、ISDB-C出力だけからこのPIDを除去します。

```ini
DiscardTsmfHeaderPackets=true
```

TSMFチャンネルではヘッダを解析して52スロットの対象TSを抽出した後、非TSMFチャンネルでは出力直前に除去します。地デジ・BS/CSには適用されません。入力側では常に保持されるため、TSMFの処理時は問題ありません。

### ログ

`DriverHost_PX4.ini` の以下でログを制御できます。

```ini
[DriverHost_PX4]
DebugLog=true
DebugLogPath="DriverHost_PX4.log"
```

`DebugLog=false` にするとログを作成しません。ログには選局した方式・周波数・復調ロック状態が出力されます。

BonDriver 側の選局ログ (ChSet から解決した周波数、周波数補正、変調方式、選局の成否) は、`BonDriver_PX-MLT.ini` の `[BonDriver]` で同じ名前の設定を使って有効にできます。既定は `DebugLog=false` で、ログファイルを作成しません。

```ini
[BonDriver]
DebugLog=true
DebugLogPath="BonDriver_PX-MLT.log"
```

`DebugLogPath` を省略すると、DLL と同じ名前の `.log` へ出力します。相対パスは DLL と同じフォルダを基準にします。ファイルは BonDriver を開くたびに作り直されるため、前回の内容を残す場合は開き直す前にコピーしてください。

## Linux

> [!WARNING]

> Linux版のISDB-C機能は実機未検証です。通常のISDB-C TS受信経路のみを実験的に含めており、WinUSB版のTSMF Relative stream抽出およびTLV生出力は未対応です。現時点では利用手順を案内する対象ではありません。

## ビルド

### Windows

Visual Studio 2022 / 2026で、次のコンポーネントをインストールしてから実行します。

- **C++によるデスクトップ開発** ワークロード
- MSBuild
- Windows 10 または Windows 11 SDK
- MSVC v143 C++ ビルドツール

> [!IMPORTANT]
> `build.ps1` は右クリックの「PowerShellで実行」でも起動できますが、失敗時にウィンドウが閉じて原因を読めません。下記のPowerShellから実行する方法を推奨します。

リポジトリの一番上のフォルダでPowerShellを開き、次の**2行を順番に**実行します。

```powershell
Set-Location .\winusb
.\build.ps1
```

1行で実行する場合は、コマンドを `;` で区切ります。

```powershell
Set-Location .\winusb; .\build.ps1
```

実行ポリシーで止まる場合は、リポジトリの一番上のフォルダから次を実行します。

```powershell
powershell -ExecutionPolicy Bypass -File .\winusb\build.ps1
```

ダブルクリックで実行したい場合は、`winusb\build.cmd` を使ってください。成功・失敗にかかわらず最後に画面を停止するため、エラーメッセージを確認できます。

`Set-Location .\winusb .\build.ps1` のように、2つのコマンドを空白だけで続けて書くことはできません。`Set-Location` が `build.ps1` を余分な引数として扱うため失敗します。

`winusb/dist/BonDriver_PX-MLT_64bit` と `winusb/dist/BonDriver_PX-MLT_32bit` に配布用ファイルが生成されます。


## 謝辞・ライセンス

ベース実装は [nns779/px4_drv](https://github.com/nns779/px4_drv) および [tsukumijima/px4_drv](https://github.com/tsukumijima/px4_drv) に基づきます。CXD2856ERのISDB-C初期化は、LinuxメインラインのSony復調器ドライバを比較資料として利用しています。

ライセンスは [GPL-2.0-only](../LICENSE) です。

## J.83B 5.6 Msps の実験機能

スカパー!プレミアムサービス光で使われる ITU-T J.83 Annex B、5.6 Msps、256QAM の設定手順を、PXMLT 系の CXD2856ER / CXD2858ER 向けに追加しています。開発側には J.83B の受信環境がなく、ロックと TS の品質は開発側では未検証です。利用者からは、スカパー!プレミアムサービス光（フレッツ・テレビ、東京）で、周波数を ChSet に直接書いてチャンネルスキャンと TVTest での視聴ができたという報告があります（2026-10）。米国向けの 5.06 / 5.36 Msps は対象外です。

### 設定と暫定的な系統の扱い

`DriverHost_PX4.ini` の対象機種のデバイス節（`Type="PXMLT"` を持つ節）に、次のキーを指定します。受信機ごとの節ではなく、デバイス節の設定です。

```ini
ExperimentalJ83B=false
```

既定値は `false` です。`true` の場合、そのデバイスの ISDB-C 選局要求が J.83B 5.6 Msps の設定へ置き換わります。ISDB-T / ISDB-S の要求は既存の経路を使います。`ExperimentalISDBC` と `ISDBCReferenceProfile` の値にかかわらず、J.83B には参照プロファイルの TS 出力設定が適用されます。

この置換は、信号による検証が済むまでの暫定的な入口です。クライアントの公開プロトコル、BonDriver、px4rec に正式な J.83B 系統を追加した設計ではありません。クライアントから見える系統名は ISDB-C のままです。

J.83B 有効時は、要求のシンボルレートと帯域幅を 5600 ksym/s と 6 MHz へ正規化します。変調方式の自動 / 64 / 256 の指定と `ForceISDBCModulation` は結果に影響せず、1回の選局要求につき J.83B の設定を1回だけ試します。自動 QAM 判定より前に変調方式を 256 へ正規化するため、既存の 64QAM → 256QAM 再試行は発生しません。

J.83B は TSMF を使わないため、TSMF mode、Relative stream、slot offset、TLV 出力のいずれかが非ゼロの要求は、ハードウェア操作前にエラーとなります。全項目ゼロは TSMF 無効の指定として扱います。黙って無視すると、指定されたストリームとは異なる TS を返す可能性があります。`DiscardTsmfHeaderPackets` による PID 0x002F 除去も J.83B では無効です。C/N は ISDB-C と同じ推定値を返します（次節）。

### チャンネル定義

J.83B の搬送波は CATV の C チャンネル（6 MHz 間隔）には乗らないため、ChSet の第6項目に中心周波数を kHz で直接書きます。スカパー!プレミアムサービス光の配置（93.25 MHz から 6.5 MHz 間隔で 587.25 MHz まで、77 波）を `BonDriver_PX4-C.ChSet.SPHD-J83B.txt` として同梱しています。`BonDriver_PX4-C.ChSet.txt` という名前に変えて BonDriver と同じ場所へ置いてください。配置は上記の受信報告に基づくもので、地域や時期によって使われる波は異なります。

- 第7項目（周波数補正）は必ず `0` を書きます。省略すると ISDB-C 用の既定補正が掛かります。
- 使われていない波はロックせず、スキャンで飛ばされます。全波を書くとスキャンに時間が掛かるので、実際に使われている範囲が分かればその範囲だけ残してください。
- **ChSet に書いていない周波数の局は、スキャンで見つかりません。** 一部の局が抜けるときは、まず書いた周波数の範囲が足りているかを確認してください。上記の報告でも、58 波だけ書いた状態では一部が抜け、範囲を広げて解消しています。

### 信号レベル（C/N）

J.83B でも ISDB-C と同じ推定値を返します。公開ドライバ `cxd2841er.c` の `cxd2841er_read_snr()` が Annex A / B / C のすべてで `cxd2841er_read_snr_c()`（bank 0x40 の 0x4c-0x4d）を呼び、16 / 64 / 256QAM に同じ換算式 `-9.50 × ln(reg / 24320)` を当てていることに基づきます。J.83B の実信号に対する値の妥当性は未確認です。

以前の版は J.83B で C/N を返さず（`-ENOSYS`）、BonDriver の `GetSignalLevel` が受信中も 0 のままでした。TVTest のチャンネルスキャン（`ChannelScan.cpp`）は、信号レベルが閾値（既定 7 dB）未満のチャンネルを、チャンネル切り替え後の待ち時間（既定 5 秒）のうちに SDT が届いた場合しか調べません。信号レベルが 0 のままだと、SDT の到着が遅いチャンネルは再試行なしで飛ばされます。古い版を使う場合は、TVTest のチャンネルスキャン設定で「信号レベルを無視する」にチェックを入れてください。

### 設定値の出典

出典は Sony / TBS 公開ドライバの `cxd28xx/cxd2878.c`、`cxd2878_priv.h`、`cxd2878.h`（GPL-2.0）です。参照箇所は次のとおりです。

- 復調器: `SLtoACC()`、`SLtoACC_BandSetting()` の `SONY_DTV_BW_J83B_5_60_MSPS` 分岐。
- IF: `iffreqConfig.configJ83B_5_60` と `SONY_DEMOD_MAKE_IFFREQ_CONFIG(3.75)`。復調器内部の 48 MHz 基準で、`round(3.75 / 48 × 2^24) = 0x140000` となります。外部水晶の 24 MHz とは別の基準で、DVB-C 6 MHz 参照プロファイルの IF 算出式と同じです。
- チューナー: `g_terr_param_table_helene` の **`SONY_HELENE_DTV_J83B_5_6` 行**と `helene_tune_terr()`。参照関数には FREIA の列挙値で HELENE 表を引く添字のずれがあるため、移植では HELENE の行名を基準に値を選んでいます。
- 遷移: 提供資料で J.83B 選局を担当する関数名は `cxd2878_set_mcns()` です。5.6 Msps の分岐と、異なるモードからの sleep 経由の遷移を参照しています。
- 復帰: `cxd2878_sleep()` の `SONY_DTV_SYSTEM_J83B` 分岐。J.83B 固有値を戻した後に既存のアナログ停止手順を使います。
- ロック: `read_status` の `SYS_DVBC_ANNEX_A / B / C` 共通分岐。bank 0x40 の 0x88 bit 0 が QAM 同期、0x10 bit 5 が TS ロックです。両方が立った場合にロックとして扱います。

待機時間の短縮や新しい再送回数は追加していません。チューナーの校正待機は既存手順と出典で共通の 50 ms を維持しています。J.83B の FEC 計数へ既存 DVB-C モニターを流用する根拠は不足しているため、J.83B のロック確認は上記のステータスだけを参照します。

### 実機確認の範囲

前提となる実機確認は、DTV02A-4TS-P で ISDB-C の DVB-C 6 MHz 参照プロファイルから TS を受信できたことです。J.83B では、その実装の構造と TS 出力設定を再利用しています。

J.83B 信号のない環境で確認できる範囲は、選局の設定処理が I/O エラーにならず非ロックで終了すること、および ISDB-C / ISDB-T に戻した後の受信です。この J.83B 切り替え・復帰の実機結果は、本変更の実装時点では取得していません。信号がないことによる選局タイムアウトと、I²C 設定失敗は別の結果です。

J.83B のロック、TS 出力、ドロップ、同期エラー、transport error、continuity error、長時間受信、複数チューナー、USB 抜去、カード通信との同時負荷は、開発側では未確認です。利用者の報告では、スキャンと視聴ができ、6時間と1時間・30分の番組を組み合わせた録画で、1時間番組1本にドロップ6が出ています（原因不明）。模擬 I²C の試験とコンパイル成功は、これらの実機試験の代わりにはなりません。Linux 版には設定の入口を追加しておらず、既存呼び出し側はゼロ初期化によって追加フラグが無効となります。

### 信号のある環境での確認手順

1. 対象機種、受信機番号、OS / アーキテクチャ、アンテナ配線、カードの配置、試験開始時刻を記録し、既存 ISDB-C / ISDB-T の受信状態と TS 計数を保存します。録画中の他の DriverHost_PX4 を停止する操作は、その録画を失わせるため避けてください。
2. 試験専用のホストと設定を用意し、対象デバイスに `ExperimentalJ83B=true` を指定します。Windows では `[DriverHost_PX4]` の `DebugLog=true` と `DebugLogPath` を設定します。macOS では起動したホストの標準エラー出力を保存します。
3. ISDB-C の通常 TS として、信号提供元が示す RF 中心周波数を指定します。BonDriver の ChSet では既存の RF 中心周波数欄を使い、TSMF の指定を含めない行を選びます。px4rec では既存の `C` と `--freq-khz` を使えます。ISDB-C で使った周波数補正値を無条件に流用せず、周波数と補正を記録してください。
4. ログの `SetFrequency` に **`J.83B 5.6 Msps (experimental, unverified)`** と対象周波数が出ること、`ISDB-C: 64QAM ... retrying 256QAM` が出ないことを確認します。TSMF 指定を拒否した場合は `TSMF parameters are unsupported`、設定処理が失敗した場合は `cxd2856er_wakeup ... failed` または `cxd2858er_set_params_c ... failed` が出ます。
5. ロックログ **`CXD2856ER J.83B: QAM sync=..., TS lock=..., status88=..., status10=...`** を確認します。非ロック時は約500 msごとに出力されます。`QAM sync=1, TS lock=1` だけで受信成功とせず、USB から届いた TS のバイト数、188バイト同期、TEI、continuity error、空受信時間を確認します。
6. 連続受信、選局変更、複数チューナー、カード通信との同時負荷、USB 切断と終了処理を試験し、D / E / S と APDU 失敗数・最大処理時間を測定できる場合は保存します。暗号方式とカードの適合性は上位層の確認事項です。
7. ISDB-T へ切り替えて受信を確認します。ISDB-C に戻す試験では、試験用ホストを終了して `ExperimentalJ83B=false` とした設定で起動し直し、元の ISDB-C 条件で TS 計数を比較します。設定はホスト起動時に読み込まれるため、INI の変更だけでは動作中のホストへ反映されません。

### J.83B のレジスタ一覧

以下は選局時の書き込みです。`SLVX` と `SLVT` は別の I²C アドレスです。SLVT の bank 選択はレジスタ 0x00 への書き込みで、表中の bank の移動ごとに発生します。値は16進数です。

| 対象 / bank | レジスタ | 値 |
| --- | --- | --- |
| SLVX / 00 | 17, 18 | 08, 00 |
| SLVT / 00 | A9, 2C, 4B, 49 | 00, 01, 74, 00 |
| SLVT / 11 | 6A | 48 |
| SLVT / 10 | A5 | 01 |
| SLVT / 40 | 21, B3, 1E, 8E | 04, 05, 80, 58 |
| SLVT / 41 | CF | 99 |
| SLVT / 11 | A3, 33–35 | 14, `00 03 3B` |
| SLVT / 00 | CE–CF | `01 01` |
| SLVT / 10 | A6–B3 | `31 A8 29 9B 27 9C 28 9E 29 A4 29 A2 29 A8` |
| SLVT / 10 | B6–B8 | `14 00 00`（IF 3.75 MHz） |
| SLVT / 40 | 14, 26–29 | 10, `07 77 07 77` |
| SLVT / 00 | 80 | 08、mask 1F |

参照プロファイルから共用する TS 出力の書き込みは、SLVT bank 00 の D3 / DE / DA bit 0 = 0、D9 = 08、32 bit 0 = 0 → 1、33 bits 1:0 = 01、bank 10 / 40 の 66 bit 0 = 1 です。serial 出力の場合のみ bank 00 の C4 / D1 bits 1:0 = 01 が加わります。TS ピンは bank 00 の 81 を出力形式に応じた mask（01 / 80 / FF）で 00 にします。選局完了処理は既存の bank 00 の FE = 01、C3 = 00 を使います。

| チューナーレジスタ | 値 |
| --- | --- |
| 01, 74 | 00, 02 |
| 87–88（開始時） | `C4 40` |
| 91–92 | `16 26` |
| 9C–9D | `00 00` |
| 5E–66 | `EE 02 1E 67 10 1C 78 08 1C`（16 MHz 水晶） |
| 62（24 MHz 水晶の場合） | 18（16 MHz の 10 に代わる値） |
| 67 | 00、mask 02 |
| 68–6F | `00 84 00 02 21 00 1B 02` |
| 70–72 | RF 中心周波数 kHz、下位バイトから3バイト。72 の上位4bitは0 |
| 73–78 | `FF 01 D9 0F 24 87` |
| 88, 87（50 ms 待機後） | 00, C0 |

復帰時は TS を停止・Hi-Z 化してから、SLVT bank 11 の A3 = 00、bank 40 の 21 = 00 / C3 = 00 / B3 = 02 / 1E = 00 / 8E = 0E、bank 41 の CF = 77、bank 40 の 14 = 1F / 26–29 = `09 9A 00 EE` を戻し、既存の ISDB-T アナログ停止処理へ進みます。停止途中の I/O 失敗では J.83B 内部状態を保持し、次の選局でも解除を再試行できるようにしています。

### 模擬 I²C 試験

`winusb/tests/j83b_driver_test.c` は J.83B のレジスタ、ロック判定、T / C 復帰、各 I/O 境界のエラー伝播を確認します。macOS のリポジトリルートからの実行例です。

```sh
clang -std=c11 -I macos/src/DriverHost_PX4 -I macos/src/common \
  -I winusb/src/DriverHost_PX4 -I winusb/src/common -I driver \
  winusb/tests/j83b_driver_test.c driver/cxd2856er.c driver/cxd2858er.c \
  macos/src/common/msg.c -o /tmp/j83b_driver_test
/tmp/j83b_driver_test
```

J.83B 無効の ISDB-C 4条件（従来 / 参照 × 64 / 256QAM）の I²C 呼び出し数と指紋も出力します。本変更の検証では、変更前のコードと4条件とも一致しました。実際の受信品質は別途、対象機種と信号での試験が必要です。
