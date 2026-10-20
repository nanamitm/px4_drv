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

ログファイルの各行の先頭には、`2026/10/17 13:31:11.123 [INFO] ...` のようにローカル時刻がミリ秒まで付きます。年月日と時分秒の書式は EDCB の `.err` に記録されるドロップ時刻と同じため、ドロップ発生時の選局・同期・FEC 計数のログを時刻で探せます。`.err` の時刻は録画側がドロップを検出して書き込んだ時刻なので、数秒程度のずれを見込んで照合してください。時刻はログをファイルへ出力する場合に付き、BonDriver 側のログの書式は変わりません。

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

### 診断ログ（取得段階・FEC 計数と TS 出力）

録画のドロップが復調器の誤り訂正の限界によるものか、ホスト内の TS 出力で失われたものかを分けるため、2種類の診断ログを用意しています。どちらも受信設定、C/N の返却値、公開プロトコル、BonDriver ABI を変えません。出力には `[DriverHost_PX4]` の `DebugLog=true` と `DebugLogPath` も必要です。

**取得段階・FEC 計数のモニター**は、デバイス節に `ExperimentalJ83BMonitor=true` を指定すると、J.83B で C/N を取得するたびに次の2行を出力します。既定値は `false` です。`ExperimentalJ83B=false` の場合は出力されません。

- `J83B monitor input=... freq=... QAMraw=... CNRraw=... IFAGCraw=...`: QAM 種別、C/N 推定器、IF AGC の生値。
- `J83B stages input=... freq=... ARraw=... TSraw=... VQraw=... ICWvalid=... INTERraw=... IQREVraw=... OffsetRaw=... TrialSRraw=... TLFraw=... TSMraw=... BERvalid=... BERerrors=... BERexp=... PERvalid=... PERerrors=... PERexp=... RSvalid=... RSerrors=...`: 取得・TS・VQ の各同期段階、インターリーブ、スペクトル反転、搬送波オフセット、シンボルレート推定、タイミング、RS 訂正前のビット誤り、パケット誤り、RS 訂正不能パケット数。

値はすべて未校正の生値で、dB や誤り率への換算は含みません。各計数は復調器の計測期間ごとの標本で、録画全体の累計ではありません。`BERvalid` / `PERvalid` / `RSvalid` が 0 の計数は、誤り 0 の証拠として扱えません。搬送波オフセットとシンボルレート推定は、取得同期（`ARraw` bit 0）が成立している場合だけ読み出します。読み取りに失敗した場合は `J83B monitor ... read failed` を警告として出力し、C/N はそのまま返します。

1回のモニター読み取りは、bank 選択と freeze の解除を含めて同期時 19 回、非同期時 16 回の I²C 操作を共有制御バスへ追加します。BonDriver が信号レベルを取得する頻度でこの操作が発生するため、同じ USB デバイスの他チューナーの受信へ影響しないことは確認していません。通常の録画環境ではこの設定を無効のままにし、問題の搬送波と正常な搬送波を同じ受信機で比較する試験に限って有効にしてください。

レジスタの位置と有効ビットの条件は、CXD2856 向けに公開されている `sony_demod_j83b_monitor.c` に基づきます。実装は aoiro-labo/px4_drv のコミット 55917e5 から移植しました。ロック判定は従来どおり前節のステータスだけを参照し、モニターの計数は判定に使いません。

**TS 出力の書き込み拒否の計数**は、受信機の StreamBuffer から RingBuffer への書き込みで失われたバイトを数えます。計数は常に行い、`ExperimentalISDBC=true` または `ExperimentalJ83B=true` のデバイスで ISDB-C の受信を止めたときだけ、次の1行を出力します。

```text
TS output diagnostics input=... freq=... receiver_max_packets=... rejected_bytes=... rejected_writes=... purge_write_discarded_bytes=... purge_requests=...
```

| 項目 | 意味 |
| --- | --- |
| `rejected_bytes` / `rejected_writes` | RingBuffer の満杯または停止中に受け付けられなかったバイト数と、その書き込み回数。 |
| `purge_write_discarded_bytes` | パージ処理中に書き込まれなかったバイト数。 |
| `purge_requests` | StreamBuffer のパージ要求の回数（失敗した要求を含む）。 |

計数は受信開始で 0 に戻り、受信停止までの区間（選局、パージ、停止を含む）の合計です。同じ受信開始のまま複数の周波数を選局した場合、表示する `freq` は最後の周波数です。PID 選別や TSMF のスロット選択で除いたパケットは、拒否として数えません。USB 転送での欠落、TS 同期の回復で読み飛ばしたバイト、名前付きパイプ以降での欠落、復調器で発生した誤りは、この計数の対象外です。値が 0 でも受信経路全体の無欠落を示すものではありません。

`J83B monitor` 行の `read_us` は、そのモニター読み取り 1 回にかかった時間（マイクロ秒）です。共有制御バスの占有時間を見積もるための値で、受信処理には使いません。

### 他受信機の Open・選局直後のバースト記録

同じ USB デバイスで別の受信機が Open（電源投入・初期化）または選局すると、録画中の J.83B 受信機で RS 訂正不能が記録されることがあります。上記のモニターは C/N の取得に合わせて約 1 秒ごとにしか読まれず、RS 訂正不能の計数は累積値ではなく直近の計測期間の値のため、短い誤りを見逃し、電源投入と選局のどちらに続いたかも区別できません。

デバイス節に `ExperimentalJ83BBurstMonitor=true` を指定すると、`ExperimentalJ83B=true` のデバイスで別の受信機の Open または選局が始まってから一定時間だけ、TS を出力中（録画中）の J.83B 受信機の状態を短い間隔で記録します。既定値は `false` です。

```ini
ExperimentalJ83BBurstMonitor=false
; 記録する時間 (500〜10000 ミリ秒、既定 3000)
;J83BBurstWindowMs=3000
; 読み取り間隔 (20〜1000 ミリ秒、既定 50)
;J83BBurstIntervalMs=50
```

出力は次のとおりです。`+N ms` は記録開始（最初の Open・選局）からの経過時間です。記録中に別の Open・選局が来た場合は記録時間が延長され、要求元を示す行が加わります。

```text
J83B burst #1 begin: trigger receiver=0 (open), window=3000 ms, interval=50 ms
J83B burst #1 +412 ms: trigger receiver=0 (tune)
J83B burst #1 +950 ms input=1 freq=132250 ARraw=5 TSraw=32 IFAGCraw=1327 RSvalid=1 RSerrors=23 read_us=...
J83B burst #1 end: input=1 freq=132250 samples=... busy=... errors=0 RSinvalid=0 RSnonzero=... RSmax=... RSsum=... IFAGC first=... min=... max=... read_us avg=... max=...
```

- 読み取るのは取得・TS 同期状態、IF AGC、RS 訂正不能計数だけです。1 回あたりの制御転送は 11 回（freeze、bank 選択、freeze 解除の書き込み 3 回と、レジスタ 4 か所の読み出し各 2 回）で、既存モニターの同期時 33 回の約 3 分の 1 です。
- 要求元の受信機自身は読みません。TS を出力していない受信機や J.83B 以外の受信機も対象外です。
- 選局・C/N 取得・Close の最中で受信機のロックを取れないときは、その回を飛ばして `busy` に数えます。記録のために受信機の処理を待たせることはありません。
- Open・選局の処理は期限を設定するだけで、I/O は専用のスレッドが行います。

共有制御バスの占有は、制御転送 1 回の往復時間に比例します。既定値（3 秒間、50 ms 間隔）で録画中の受信機 1 台を記録すると、1 回の Open・選局につき約 60 回 × 11 転送 = 約 660 回の制御転送が加わります。往復時間は環境によって異なるため、まず `read_us` の実測値から占有率を確認してください。この制御転送は、同じデバイスで選局中の受信機の I2C 操作やカードリーダーの通信と同じ制御経路を使うため、選局時間がわずかに延びる可能性があります。記録そのものが現象に影響していないかを確かめるには、`false` の場合とドロップの発生を比較してください。

### バックエンド電源の保持

PX-MLT8PE は 1 枚の基板に 3 チューナー側（`PXMLT8PE3`）と 5 チューナー側（`PXMLT8PE5`）の 2 つの USB デバイスを持ち、それぞれが独立してバックエンド電源を投入・切断します。再現試験では、3 チューナー側で J.83B を受信中に 5 チューナー側の最初の受信機を開いて電源を投入すると、20 回中 19 回で 3 チューナー側に CC エラーが記録されました。逆向き（3 チューナー側の電源投入と 5 チューナー側の受信）と、電源の切断では記録されませんでした。

デバイス節に `HoldBackendPower=true` を指定すると、そのデバイスはホストの初期化時に電源を投入し、終了または USB 切断まで切りません。既定値は `false` で、従来どおり最初の受信機またはカードの利用者が投入し、最後の利用者が閉じたときに切ります。

```ini
HoldBackendPower=false
```

- 保持は、受信機・カードと並ぶ電源の利用者として数えます。保持中は受信機やカードを閉じても電源を切らず、開くときも電源の投入を行いません。
- 初期化時の投入に失敗した場合は保持せず、従来どおりの投入・切断に戻ります。
- USB の切断で保持は外れ、再接続後の初期化で改めて投入します。
- 保持中は受信機を使っていない間も基板へ給電するため、その分の消費電力が増えます。

同じ基板の両方のデバイスを使う構成では、相手側の録画に影響した側（試験では 5 チューナー側）に指定します。5 チューナー側に指定した同じ再現試験（3 チューナー側で 327.25 MHz を受信、各操作 5 回、約 7 分）では、CC エラーは 0 件でした。指定後の効果は、受信機の組み合わせを固定した再現試験と、録画時のドロップで確認してください。

同梱の `BonDriver_PX-MLT.ini` は、3 チューナー側の受信機定義に `OpenPriority=-1` を指定し、5 チューナー側を先に開きます。5 チューナー側で受信中の再現試験では、3 チューナー側の電源投入と、5 チューナー側の別受信機の Open・選局のどちらでも CC エラーは記録されませんでした。3 チューナー側を使う構成では、この節の設定を併用してください。

### チューナーの過大入力検出レベル

CXD2858ER の選局設定のうち、0x68 から書き込む 17 バイトの 14〜15 バイト目は、地デジで `0x99 0x00`、J.83B で `0xd9 0x0f` です。Sony / TBS 公開ドライバの表の並びから RF / IF の過大入力検出レベルと推定していますが、データシートでの確認はできていません。J.83B の受信機では、同じデバイスの別受信機の選局や電源投入の直後に C/N が一時的に約 6 dB 下がり、256QAM の所要 C/N を下回った時刻にドロップが記録されています。この設定は、検出レベルの違いがその落ち込みに関係するかを比較するためのものです。

デバイス節に `ExperimentalJ83BTerOverloadLevel=true` を指定すると、`ExperimentalJ83B=true` で J.83B を選局するときだけ、この 2 バイトに地デジの値を書き込みます。既定値は `false` で、J.83B の値を使います。地デジ・BS/CS の選局と、`ExperimentalJ83B=false` の ISDB-C の選局は変わりません。

```ini
ExperimentalJ83BTerOverloadLevel=false
```

- 選局ごとに書き込むため、設定の変更はホストの再起動後の選局から反映されます。
- 平常時の C/N や RS 訂正不能計数が悪化しないかを、`false` の場合と同じ周波数で比較してください。

### 地デジの受信機を閉じるときのチューナー停止

同じデバイスで J.83B を受信中に、別の受信機で受信していた地デジを閉じると、J.83B 側に RS 訂正不能が記録されることがあります。J.83B の受信機を閉じる場合、`cxd2858er_stop_t` は ISDB-C を対象外としてチューナーを停止しませんが、地デジの受信機ではチューナー停止のレジスタ書き込みを行います。

デバイス節に `ExperimentalKeepTunerOnClose=true` を指定すると、同じデバイスの別受信機が使用中の場合に限り、地デジの受信機を閉じるときのチューナー停止を省きます。既定値は `false` です。

```ini
ExperimentalKeepTunerOnClose=false
```

- 停止を省いたチューナーは、J.83B の受信機を閉じた場合と同じく、その受信機の次の Open で電源投入から初期化し直すか、バックエンド電源の切断まで動作を続けます。`HoldBackendPower=true` のデバイスでは、次の Open まで動作を続けます。
- 復調器のスリープは従来どおり行います。
- PX-MLT8PE の 5 チューナー側で受信機 1 を録画役、受信機 2 を操作役にした再現試験（各操作 5 回）では、地デジの Close の直後に CC エラーが記録された回数が、`false` の 5 回（計 15 件）から `true` の 0 回になりました。別受信機の Open 直後の最初の選局による CC エラーは、この設定では変わりませんでした。

### チューナーを動作させたままにする

再現試験では、同じデバイスの別受信機を Open するだけ、または開いたまま選局し直すだけでは、受信中の J.83B に CC エラーは記録されませんでした。記録されたのは Open 後の最初の選局で、`cxd2858er_init` の直後に停止状態にあるチューナーの RF 部が動作を始めるときです。地デジの受信機を閉じるときの停止 (前節) は、その逆の変化にあたります。

`HoldBackendPower=true` のデバイス節に `ExperimentalWarmTuners=true` を指定すると、ホストの初期化時に全チューナーを一度選局して動作状態にし、以後は Open でチューナーを初期化し直さず、Close でも停止しません。Open 後の選局は周波数の変更だけになります。既定値は `false` で、`HoldBackendPower=false` の場合は効果がありません。

```ini
ExperimentalWarmTuners=false
```

- 初期化時の選局先は 93 MHz です (J.83B が有効なら J.83B、無効なら地デジの設定)。復調器はスリープへ戻すため TS は出力しません。
- 電源の投入・切断と USB の切断で動作状態の記録を消し、次の Open で従来どおり初期化します。初期化時の選局に失敗した受信機も、従来どおり Open で初期化します。
- 使っていない間もチューナーが動作するため、その分の消費電力が増えます。
- PX-MLT8PE の 5 チューナー側で受信機 1 を録画役、受信機 2 を操作役にした再現試験 (各操作 5 回、約 7 分) では、CC エラーは `false` の 138 件 (Open 後の選局で毎回記録) から `true` の 0 件になりました。録画ソフトでの長時間の受信、ほかの受信機の組み合わせ、5PE・8PE3 での効果は未確認です。

### チューナーの入力レベル

C/N は復調器が一定時間平均した推定値で、入力レベルが強すぎるのか弱すぎるのかは判断できません。デバイス節に `ExperimentalTunerRssi=true` を指定すると、`ExperimentalJ83BMonitor=true` の J.83B モニターの読み取りに合わせて、チューナー (CXD2858ER) の IF AGC・RF AGC の生値と、入力レベルの推定値を `Tuner RSSI` 行としてログへ出します。既定値は `false` です。

```ini
ExperimentalTunerRssi=false
```

- 読み取り手順と換算式は Sony / TBS 公開ドライバ (`cxd2878.c`, GPL-2.0) の `ascot3_read_rssi()` を参照しています。CXD2858ER の地上・ケーブル側は同じレジスタ配置を使いますが、換算表は CXD2858ER 用に校正されていません。推定値 (`est`) は、配線の変更前後のような同じ受信機・同じ周波数での相対比較に使ってください。dBμV は 75 Ω 換算の目安です。
- 読み取りの間、チューナーの 0x59・0x87・0x88 を一時的に切り替え、読み取り前の値に書き戻します。1 回あたり I2C の読み書きが 12 回増えます。

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
