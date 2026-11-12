# PX-S1UD の WinUSB／BonDriver 対応

対象は PLEX PX-S1UD（USB ID `3275:0080`）、地上デジタル放送の1チューナーです。
Windows の DriverHost_PX4 が WinUSB を直接制御するため、libusb DLL や
sianod の起動は不要です。Linux カーネル版と macOS 版への対応は対象外です。

## 導入

1. 配布物の `Driver` にある証明書の導入手順に従い、デバイスマネージャーから
   `PX-S1UD.inf` を指定して WinUSB ドライバーをインストールします。
   この INF の DeviceInterfaceGUID がホストの検出条件になるため、Zadig で
   WinUSB を導入済みの場合も、この INF で更新が必要です。
2. 利用するアプリと同じビット数の `BonDriver_PX-S1UD_32bit` または
   `BonDriver_PX-S1UD_64bit` を展開し、DLL・INI・ChSet・DriverHost と
   `firmware`、`licenses` を含む構成を維持して配置します。
3. TVTest などで `BonDriver_PX-S1UD.dll` を選択し、地デジのチャンネルスキャンを行います。

`firmware/isdbt_rio.inp` は同梱済みです。DriverHost は、存在するファイルを次の順序で検索します。

1. `DriverHost_PX4.ini` の SIANO 定義で指定した `FirmwarePath`
2. `DriverHost_PX4.exe` と同じフォルダーの `isdbt_rio.inp`
3. `DriverHost_PX4.exe` の配置先にある `firmware/isdbt_rio.inp`

`FirmwarePath` の相対パスはホスト EXE の配置先が基準です。指定先が存在しない場合は
後続の候補を探すため、既定の INI を変更せず EXE と同じフォルダーへ置く方法も使えます。
カレントディレクトリーは検索対象外です。選択したファイルの内容が不正な場合は、
別ファイルへ切り替えず読み込みエラーとなります。
コールド起動でファイルが不足・不正の場合は初期化に失敗します。

PX-S1UD に内蔵カードリーダーはありません。スクランブル解除を行う構成では
外付けカードリーダーと、利用するアプリに対応した設定が必要です。

## 受信と終了

選局時は USB 受信を継続し、TS バッファの世代を進めます。
Siano のメッセージ途中で read を取り消すと、再開後に残りの TS をヘッダーとして読む
ことがあるため、選局ごとの受信停止・pipe の flush は不要です。
ロック確定前に投入した受信要求と、前の capture 世代に属する要求は配信対象外です。
BonDriver の `FramedStream=true` は、パイプ内に残った旧世代の TS も除外するために必要です。
受信の端数は188バイトの TS パケットへ連結し、制御ヘッダーは出力から取り除かれます。
全 PID の受信を要求し、null パケットと同期後の TEI パケットも維持します。

制御のタイムアウト・通信異常・配信バッファのあふれはログへ記録され、受信は失敗状態に
移ります。要求と応答の対応を保証できない状態からの自動復旧はなく、再オープンが必要です。
ログの `packets`、`tei`、`discarded`、`malformed`、`ret` が調査の入口になります。
CNR は firmware の SNR（dB）をホストの milli-dB 単位へ変換します。
統計を提供しない旧 firmware では CNR は0です。

### ドロップ調査用ログ

DriverHost_PX4.ini の `[DriverHost_PX4]` と BonDriver_PX-S1UD.ini の `[BonDriver]` で
`DebugLog=true` にすると、次の2か所で TS を変更せず連続性を観察できます。

- `stage=siano_usb_input`: Siano フレームと TS 境界を解釈した後、ホストの TS バッファへ書く直前。
- `stage=bondriver_output`: BonDriver のキューから取り出し、TVTest などへ返す直前。

`TS anomaly` 行にはミリ秒単位の採取時刻 `observed_at`、PID、異常種別、前の CC、期待する CC、実際の CC、
世代内のパケット番号が記録されます。異常種別は `cc_jump`、`same_cc_changed`、
`duplicate_run`、`adaptation_cc`、`tei`、`sync`、`malformed` です。
同一パケットの1回の再送と、その PCR だけが変わる再送は CC 異常として数えません。
discontinuity indicator、選局による世代変更、BonDriver のパージでは比較の基準をリセットします。
188バイト境界を失うと `sync` を1件記録し、次の `0x47` まで読み飛ばして解析を続けます。
読み飛ばしたバイト数は集計行の `resync_bytes` に残り、欠落区間をまたぐ CC 比較は行いません。

通常は10秒ごとに `TS diagnostics` の集計行を出力し、パージ・終了時にも集計します。
大量エラーでログ書込みが受信を妨げるのを避けるため、異常の詳細は各段階で1秒あたり32件までです。
省略件数は `suppressed`、異常の累積数は集計行に残ります。
受信・配信スレッドからのファイル書込みはなく、固定長の診断キューを専用スレッドが処理します。
診断キューはロックを使わない固定長のリングで、満杯の場合だけ待機せずに `queue_dropped` へ省略件数を記録します。
この値が0でない区間では診断行自体に欠けがあるため、異常のログがないことだけで正常とは判断できません。
行頭の時刻はログ書込み時刻なので、段階間の比較には `observed_at` を使います。
記録されるのはヘッダー由来の数値だけで、映像・音声・カード通信の内容はログに含まれません。

USB入口の `gap_us` は直近の読取要求再投入間隔と現在の処理待ち時間の大きい方、
`max_gap_us` は集計区間の最大値です。`ready_reads` は異常を記録した時点で完了通知済みの
他の読取要求数で、USBハードウェア内の要求数や、欠落したパケット数そのものではありません。
BonDriver 出力の `ready_reads=-1` は測定対象外です。
USB入口の `framer_discarded` は、TS 境界の解釈で同期を取り直すために捨てたバイト数を、
その世代の最初の出力時点からの差分として記録します。この値が増えた直後の `cc_jump` は、
USB フレームの内容が TS 境界を保っていなかったことを示します。値が増えない `cc_jump` は、
TS 境界の解釈より前に、パケット単位で欠けていたことを示します。BonDriver 出力では `-1` です。段階間の世代番号とパケット番号は独立なので、
異常の PID・CC の並びと時刻を突き合わせます。キューの分だけ出力側の時刻が遅れることがあります。

両段階で同じ異常が見える場合は、USB入口より前から異常があったことを示しますが、
電波・チューナー／firmware・USB転送のどこが原因かは、それだけでは確定できません。
出力側だけの異常は、ホストのバッファ・パイプ・BonDriverのキューを含む区間の調査材料になります。
このログ設定は各プロセスの次回起動時に有効になります。

DriverHost_PX4 のログには受信品質も記録されます。統計はアプリケーションが信号レベルを取得したとき
（最短100ミリ秒間隔）の既存要求から読み、TS と同じ USB 経路の制御転送は増やしていません。
信号レベルを取得しないアプリケーションでは記録されません。
firmware 8.1 の実機では、階層ごとの誤りパケット数・総パケット数・TMCC 誤りはロック時に0へ戻る累積値でした。
そのためログは前回応答からの増分を `+増分(累積値)` の形で記録し、値が減った応答はロックし直しによる再計数として扱います。
累積値なので、統計の取得間隔より短い誤りも次の応答の増分に現れます。
`PX-S1UD: signal errors` 行は、復調のロック外れ、または誤りパケット数 `err`・TMCC 誤り `tmcc` が
増えた応答ごとに出力されます。`snr` は dB、`rssi` は dBm、`ber` は 1E-5 単位で、`-1` は値なしです。
`PX-S1UD: signal summary` 行は約10秒ごとの集計で、階層 A・B・C の `err` と `total` の増分を合計します。
統計の解釈は Linux smsdvb の `sms_isdbt_stats_ex` に基づくため、firmware 8.0 未満では記録されません。
`err` が増えた時刻に `siano_usb_input` の `cc_jump` があり `tei` がない場合、
firmware が誤り訂正できなかったパケットを TS から除いた可能性を示します。

`siano_usb_input` で異常を検出すると、`TS transfer` 行に USB 転送単位の記録が出力されます。
異常を含む転送 (`tag=trigger`) の直前15件 (`tag=before`) と直後8件 (`tag=after`) が対象で、
`seq` は完了順の通し番号、`completed_at` は完了を検出したローカル時刻（マイクロ秒）、`slot` は読取要求の番号です。
`bytes` は USB から受け取った長さ、`frame_length` は Siano のメッセージ長で、`bytes` が上回る分は解釈されません。
`packets` はその転送で TS 境界の解釈から出力されたパケット数、`first`・`last` は先頭と末尾パケットの PID/CC です。
隣り合う転送の `last` と `first` の CC、`seq` と `slot` の並び、`completed_at` の間隔から、
転送単位の欠落、入替え、転送内の欠落を区別します。出力は1秒あたり64件までで、省略数は次の行の `skipped` に残ります。

## J.83B の受信（実験的）

PX-S1UD のチューナーは、スカパー！プレミアムサービス光の J.83 Annex B（256QAM、5.6 Msps）の
周波数帯も受信できます。復調前の IQ（12 MS/s）を USB で送出する J.83B 用の firmware を読み込むと、
ホスト側で復調して TS を得られます。J.83B 用の firmware は Siano の通常の firmware とは別のファイルで、
出所と再配布の条件を確認できていないため配布物に含まれず、利用者が用意したファイルを指定します。

### 構成

DriverHost は USB を持ち続け、J.83B の選局で次の順に処理します。

1. J.83B 用の firmware を読み込みます。ISDB-T の選局へ戻るときは、通常の firmware を読み込み直します。
2. IQ の送出を開始する要求（`0x037A`）を、中心周波数（Hz）を引数として送ります。この要求が選局を兼ね、
   ISDB-T の選局要求は送りません。引数0で送出を止めます。
3. IQ のブロック（`0x037C`、7,360 サンプルとブロック番号）を、helper の標準入力へ渡します。
4. helper の TS を、ISDB-T と同じ配信バッファへ書きます。

復調と誤り訂正は、MSi2500 と同じ `msi2500_frontend.exe` の `serve-s1ud` が行います。
DSP を別プロセスに置く理由は MSi2500 と同じで、ライセンスと障害の分離のためです。
helper はブロックの間で落ちる 4〜7 サンプルの長さを判定して時間軸を作り直し、
I と Q の利得のずれを補正してから J.83B を復調します（`tools/msi2500_frontend/src/s1ud.rs`）。

firmware の切り替えでは、SW_RELOAD_START を送った後の状態を GET_VERSION で確かめます。
実機では、通常の firmware は読み込みを待つ状態になり、J.83B 用の firmware は ROM のブートローダー
（モード255）へ戻ることがありました。後者では bulk IN の受信が一度 ERROR_GEN_FAILURE で失敗するため、
読み込み直しの間だけ IN パイプをリセットして受信要求を出し直し、ROM へ戻っていればコールド起動の手順で書きます。
J.83B 用の firmware は INIT_DEVICE の後も独自のモード（実機では7）を返すため、起動後のモードは確かめません。
J.83B 用の firmware が稼働したままの機器を開いた場合は、通常の firmware を読み込み直してから ISDB-T として使います。

helper の TS が3秒以内に届いていればロックとみなします。CNR の値は、helper が状態の
フレームで報告する復調器の MER (dB) です。RS 復号の成功割合は正常受信中にほぼ 100% のまま変わらず、
受信の余裕を読み取れないため、C/N の表示には使いません。MER は判定帰還で追従している間の
シンボルの判定誤差から求めた値で、受信機の雑音と等化の誤差を含むため、RF の C/N とは一致しません。
検証環境の 138.75 MHz では 31〜32 dB でした。MER を報告しない古い helper では、従来どおり
RS 復号の成功割合から求めた 0〜100 の品質点を返します。
firmware の受信統計は ISDB-T のもののため、J.83B では使いません。

チップは IQ を USB へ送り切れずにブロックを落とすことがあり、BonDriver_S1UD.dll でも約2%でした。
helper はブロック番号の欠落を検出し、8ブロック以下ならつないで復調を続け、それを超えると作り直します。
作り直した区間では TS が途切れ、continuity counter の不連続として現れます。
DriverHost と helper の間のキューがあふれた場合も、捨てたブロックは同じく欠落として扱われます。

### 設定

`DriverHost_PX4.ini` の `[DeviceDefinition18.Config]` に J.83B 用の firmware と helper を指定し、
`[DeviceDefinition18.Receiver0]` の `System` を `"ISDB-T,J.83B"` にします。
相対パスは `DriverHost_PX4.exe` の配置先が基準です。helper は MSi2500 の配布物の
`msi2500` フォルダーにあり、PX-S1UD の配布物には含まれていません。

```ini
[DeviceDefinition18.Config]
FirmwarePath="firmware\isdbt_rio.inp"
J83BFirmwarePath="firmware\s1ud_j83b.inp"
HelperPath="msi2500\msi2500_frontend.exe"
```

`BonDriver_PX-S1UD.ini` では、`[BonDriver]` と `[ReceiverDefinition0]` の `System` を
`"ISDB-T,J.83B"` にし、`[BonDriver.J.83B]` の `ChSetPath` に `BonDriver_PX4-J83B.ChSet.txt` を指定します。
`System` に J.83B を書いて `J83BFirmwarePath` を指定しない場合、DriverHost は機器を登録しません。

helper の標準エラー出力は、`DriverHost_PX4.exe` の配置先の `px-s1ud_j83b-<機器番号>.log` に残ります。
終了時の行の `queue_dropped_blocks` は DriverHost と helper の間で捨てたブロック数、
`gaps` はチップとキューで落ちたブロック数、`seg` は作り直した回数に1を加えた値です。

### 処理量

138.750 MHz の収録（29秒）で測った helper の前処理と復調は、Ryzen 9 5900X の1スレッドで
x64 が実時間の約0.64倍、x86 が約0.68倍でした。誤り訂正は別スレッドで約0.45倍（x64）です。
遅い CPU では実時間に間に合わない可能性があります。

### 検証範囲

Windows 上の PX-S1UD 1台で、138.750 MHz（スカパー！プレミアムサービス光）を `siano_live_test` の
J.83B モードで受信しました。DriverHost と同じ受信バックエンドを直接呼び出す試験で、
各回とも開始時に J.83B 用の firmware から通常の firmware へ戻し、選局で J.83B 用の firmware へ切り替えています。

| 項目 | x64 30分 | x64 60秒 | x86 60秒 |
| --- | --- | --- | --- |
| 受信パケット | 48,073,646 | 1,636,079 | 1,629,619 |
| ロック外れ（1秒ごとの確認） | 0 | 0 | 0 |
| 同期／TEI／形式エラー、書込み拒否 | すべて0 | すべて0 | すべて0 |
| 連続性エラー | 3,655 | 43 | 88 |
| MER | 31.9 dB | 31.7 dB | 31.7 dB |
| チップが落としたブロック | 3,196（0.11%） | 2 | 148 |
| 復調の作り直し | 222 | 0 | 4 |
| DriverHost と helper の間で捨てたブロック | 0 | 0 | 0 |
| RS 復号の訂正不能 | 26,681／84,697,241 | 293／2,882,178 | 344／2,871,004 |

連続性エラーの大半は、helper が欠落をつなげずに復調を作り直した区間で生じています。
30分の試験では作り直しが約8秒に1回あり、そのたびに TS が途切れました。

ブロックの欠落はチップの中で起きています。IQ の送出を止める要求の応答には firmware が数えたバッファーの統計が入り
（BonDriver_S1UD.dll のログの書式に合わせると、溜めた数、送った数、捨てた数など）、3分の試験で捨てた数295は
ブロック番号から数えた欠落と一致しました。ブロック番号は捨てた分も進むため、溜めた後、USB へ送る前に捨てられています。
その間もホストの受信要求は32本中8本以上が残っており、受信要求を128本に増やしても欠落は減りませんでした。
DriverHost は IQ の送出を止めるときに、この統計とホストの受信要求の余裕をログへ記録します。

欠落は約5ミリ秒の間に「1ブロック届いて2〜7ブロック落ちる」が数回続く形で起きます。helper は1回ごとの欠落を
つなごうとしますが、次の理由で固まりの途中で判定に失敗します。

- 15サンプル（7シンボル）の整数倍のずれは判定誤差に現れないため、4ブロック以上の欠落では候補が一つに定まりません。
- 長い区間を埋める間に搬送波の位相とタイミングがずれ、正しい候補の判定誤差も他と区別できなくなります。

再同期の短縮と判定の改善は今後の課題です。

収録データでの確認として、BonDriver_S1UD.dll 経由で収録した USB データを実時間の速度で `serve-s1ud` へ渡し、
キューの欠落なく 771,742 パケットを復号しました。同じデータを DriverHost の helper 制御部
（`siano/iq_helper.cpp`）経由で渡し、同じ TS が戻ることも確認しました。
`siano_frontend_test` では、firmware の切り替え（ROM へ戻る場合を含む）、IQ の送出要求の形式を模擬試験しています。

BonDriver 経由の視聴と録画、138.750 MHz 以外のチャンネル、選局を続けたまま ISDB-T と J.83B を
交互に切り替える試験は、まだ確認していません。
実機試験は `siano_live_test` の J.83B モードで行えます。INI の `[PX-S1UD]` 節に
`J83BFirmwarePath` と `HelperPath` を書き、中心周波数を kHz で指定します。

```powershell
build/x64/Release-static/siano_live_test.exe j83b:138750 3600 build/s1ud-j83b-138750.ts build/s1ud-j83b.ini
```

## 実装と検証範囲

Siano 制御は [siano-userland](https://github.com/Khronos31/siano-userland/tree/2f32c2a34665d6ccca39c9362eb786be78351347)
を参照しています。出典、変更点、ソースと firmware のライセンスは `licenses/siano` にあります。

`siano_frontend_test` は起動済み機器への初期化再送回避、firmware 転送・再ロード、
選局、全 PID 設定、異常応答、統計、split フレーム、TS の端数、旧受信世代の遅延完了を模擬試験します。
この移植版の短時間実機受信は下記の範囲で確認済みです。
13ch／14ch 間の短時間切替も下記の範囲で確認済みです。
他のチャンネル、USB の抜き差し、長時間受信は別途確認が必要です。
参照実装の実機検証結果は、この移植版の検証結果には含まれません。

### Windows x64、13ch の30秒受信（2026-10-29）

Windows 上の PX-S1UD 1台（WinUSB、firmware 8.1）を、DriverHost と同じバックエンドを
直接呼び出す `siano_live_test` で試験しました。既存の TVTest と DriverHost は継続動作のままです。
アンテナ入力から13chの放送 TS を受信し、カードリーダーと復号処理は使用していません。

| 項目 | 結果 |
| --- | --- |
| 物理チャンネル／中心周波数 | 13ch／473.142857 MHz |
| 試験時間 | 30秒 |
| CNR（30回の統計取得） | 29〜30 dB |
| 保存パケット／バイト数 | 350,600／65,912,800 |
| 同期／TEI／連続性／形式エラー | すべて0 |
| ホストの書込み拒否バイト数 | 0 |
| 停止処理／ファイル書込み | 正常 |
| TSDuck による保存 TS の再検査 | 同期・TEI・全 PID の連続性・重複エラー0、188バイト端数0 |

TS の SHA-256 は `9db3ba475971c8526e843c2974079b8b828a7406933a1a08ca225c0d41c8b36b` です。
この試験は x64 の受信バックエンドと TS 保存の確認であり、BonDriver 経由の画面表示・復号、
コールド起動時の firmware 転送、x86 の実機受信、長時間動作の確認は含まれません。
実機試験ツール自体は x86／x64 の双方でビルド済みです。

`winusb` をカレントディレクトリーにした再試験の例です。
テスト EXE の配置先にも `firmware/isdbt_rio.inp` が必要です。

```powershell
msbuild tests/siano_live_test.vcxproj /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143
New-Item -ItemType Directory -Force build/x64/Release-static/firmware
Copy-Item pkg/DriverHost_PX4/firmware/isdbt_rio.inp build/x64/Release-static/firmware/
build/x64/Release-static/siano_live_test.exe 13 30 build/siano-ch13-30s.ts
tsanalyze --isdb --japan --normalized build/siano-ch13-30s.ts
```

### 13ch／14ch の交互選局（2026-10-29）

同じ受信機を開いたまま capture と読み取りスレッドを継続し、各回のロック確定後に
3秒ずつ受信しました。TS は受信世代ごとに分けて保存し、全 PAT の CRC と TSID、
全 PID の連続性・重複を検査しています。13ch の TSID は31761、14ch は31762です。
TSDuck による検査とは別に PAT を組み立て直し、誤った CRC の PAT を拒否することも確認しました。

初回の x64 試験では26回目の14ch受信で連続性エラー12件を検出しました。
x86 試験では6回目の選局時、USB 受信の停止・再開直後に3,760バイトのデータを
正規の Siano フレームとして解釈できず、選局に失敗しました。
選局ごとの read 取消・pipe flush を除き、USB 受信を継続しながら Gate とリングの世代で
旧 TS を除外する方式へ修正しました。初回の連続性エラーの発生源は確定していませんが、
修正後の以下の試験では再発していません。

| 項目 | Windows x64 | Windows x86 |
| --- | --- | --- |
| 選局回数 | 40（各 ch 20回） | 20（各 ch 10回） |
| 開いたままの切替回数 | 39 | 19 |
| 保存 TS パケット数 | 1,376,704 | 686,000 |
| 選局開始からロック確定 | 1,752〜1,916 ms | 1,747〜1,901 ms |
| 選局開始から最初の TS | 1,808〜1,974 ms | 1,802〜1,960 ms |
| CNR | 27〜30 dB | 26〜30 dB |
| PAT CRC／切替先 TSID | 1,202 sections、すべて一致 | 601 sections、すべて一致 |
| 同期／TEI／連続性／重複／形式エラー | すべて0 | すべて0 |
| 書込み拒否・ファイル書込み・停止処理 | 拒否0、正常終了 | 拒否0、正常終了 |

通常の DriverHost も両アーキテクチャで再ビルド済みです。
`siano_frontend_test`、`receiver_tune_completion_test`、`stream_epoch_test`、
`ringbuffer_purge_test` は x86／x64 とも成功し、配布検査7件も成功しました。
既存 TVTest と DriverHost は継続動作のままで、この試験は受信バックエンドを直接使っています。
BonDriver 経由の TVTest 操作、復号・画面表示、長時間録画の検証は含まれません。

```powershell
msbuild tests/siano_switch_test.vcxproj /p:Configuration=Release-static /p:Platform=x64 /p:PlatformToolset=v143
build/x64/Release-static/siano_switch_test.exe 13 14 40 3000 build/siano-switch-13-14
python tests/siano_switch_analyze.py build/siano-switch-13-14 40 --tsid-a 31761 --tsid-b 31762
```

TSID は受信地域・搬送波で異なるため、再試験時には各チャンネルの単独受信結果から指定します。
