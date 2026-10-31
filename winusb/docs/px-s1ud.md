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
