# PX-BCUD の Windows WinUSB 対応（実験版）

## 対象と構成

対象機器は PLEX PX-BCUD、USB VID/PID `3275:0085`、EM28178 USB ブリッジ、
TC90532 復調器、QM1D1C0045 系チューナー（識別値 `0x68`）です。
2026-10-28 の実機は revision `0205`、USB High-Speed、interface 0、
alternate setting 1、isochronous IN endpoint `0x84`、940 bytes / microframe、interval 1 でした。
alternate setting 0 の同 endpoint は最大長0で、bulk endpoint はありません。

Windows 10 / 11 用の実装は `winusb/src/DriverHost_PX4/pxbcud/` にあります。
`DeviceBase`、`ReceiverBase`、`ReceiverManager` と既存の BonDriver を利用し、
EM28178 の通信と機種固有の初期化は IT930x の受信経路から独立させています。
Linux / macOS 版の PX-BCUD 対応は含まれていません。

`Type="PXBCUD"` のデバイス定義がある場合だけ登録されます。
通常配布の INF は `winusb/pkg/inf/PX-BCUD.inf`、BonDriver の設定は
`winusb/pkg/BonDriver_PX4/BonDriver_PX-BCUD.ini`、機種定義は共通の
`winusb/pkg/DriverHost_PX4/DriverHost_PX4.ini` にあります。
通常の `winusb/build.ps1` で `dist/BonDriver_PX-BCUD_32bit` と
`dist/BonDriver_PX-BCUD_64bit` を作成し、共通の `dist/Driver` に INF と署名済みカタログを収録します。
配布物の作成は公開リリースとは別の工程で、未確認項目の検証完了までリリースを保留しています。

## 制御と受信

USB vendor control transfer の request 0 をブリッジレジスター、request 2/3 を
I2C 転送に用います。バス1を選択し、復調器 `0x15` の `0xfe` 経由で
チューナー `0x61` を操作します。短転送はエラーになり、I2C ACK 不応答、
タイムアウト、切断は別の戻り値になります。Win32 の USB エラーには転送情報を記録します。
操作ごとに固定の絶対期限があり、USB の取消後も OVERLAPPED とバッファを回収します。

受信は 5 requests x 64 microframes とし、WinUSB の登録済み isochronous バッファを使います。
最初の転送以外で `ContinueStream=TRUE` とするため、継続する帯域を確保できなければ
明示的なエラーになります。転送ごとの Status / Offset / Length を確認し、エラーの
microframe と TS 同期損失、TEI を別に記録します。USB エラー時は端数と同期状態を破棄します。
5秒間データを受信できなければワーカーが失敗を通知します。

TS の境界獲得は既存の `driver/ts_sync.h` を使い、188バイト周期を3個確認します。
境界獲得後の TEI は上位へ渡します。選局前に USB I/O を回収し、残った端数を破棄して
StreamBuffer の epoch を更新するため、前のチャンネルの TS を持ち越しません。
機器内部の FIFO に残る旧 TS は、CRC が正しく選択 TSID と一致する PAT を確認するまで
配信から除外し、除外数を `startup_discarded_packets` として記録します。
PAT の確認には分割セクション、continuity counter、adaptation field、current_next を含めます。
選局中は USB ワーカーだけを止め、配信側の読取接続とリングの寿命を維持します。
BonDriver の設定は `FramedStream=true` とし、配信パイプ内に残る旧 epoch のバッファも
既存の世代管理で破棄します。旧形式の raw pipe では世代情報を伝えられないため、
機種側の同期と PAT 検証だけでは切り替え時の混入を防げません。
BonDriver の初回 Open 時点では未選局のため、capture 要求は保持して選局完了後に USB 受信を開始します。

## 参考資料と実機で判明した差

- [knight-rider/ptx](https://github.com/knight-rider/ptx/tree/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1)
  の `drivers/media/usb/em28xx/pxbcud.patch`、`em28xx-dvb.c`、
  `drivers/media/tuners/qm1d1c004x.c`、`drivers/media/dvb-frontends/tc90522.c`。
  基板初期化、I2C 接続、チューナー設定表と選局の参考です。
- [Linux v4.5 em28xx](https://github.com/torvalds/linux/tree/v4.5/drivers/media/usb/em28xx)
  の `em28xx-core.c`、`em28xx-i2c.c`、`em28xx.h`。
  USB setup packet、I2C 状態確認、TS capture bit、バッファ数・転送単位の参考です。
- [Linux v4.8 qm1d1c0042.c](https://github.com/torvalds/linux/blob/v4.8/drivers/media/tuners/qm1d1c0042.c)。
  ソフトリセットと全レジスター初期化を参照しています。
- [WinUSB isochronous 転送](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_readisochpipeasap)。
  バッファ登録、フレーム境界、継続転送、パケット状態の扱いの参考です。

ptx の簡略版は初期値の多くをメモリーへ保持するだけで、実機への全書込みがありません。
そのままの移植ではロックと TSID 選択が通っても受信は安定せず、実測 C/N は約6dBで
TEI付き候補が続きました。Linux 本体のリセットと全初期値書込みを補うと約17.3dBへ改善し、
TEIなしの TS を受信できました。

TS 出力についても ptx の probe 値 `1c=15,1f=04` を残すと isochronous のデータ長が0でした。
既存 px4_drv の TC90522 並列 TS 出力設定 `1c=00,1f=00` とブリッジの capture bit を
組み合わせると実機の TS が流れます。停止時はブリッジを先に止め、`1c=80,1f=22` に戻します。
停止途中のエラーがあっても、残る停止操作を試みます。

### 高負荷時のスケジューリング

アイソクロナス転送はフレーム番号を追い越されると転送全体が失敗し、再投入では取り返せません。
受信は5要求 x 64マイクロフレームで、キューの余裕は40msです。
この時間より長く受信スレッドが実行されないと、全パケットが `USBD_STATUS_ISO_NOT_ACCESSED_BY_HW`
(`0xC0020000`) になり、復旧後も負荷が続く限り数百msで再発して復旧予算を使い切ります。

論理プロセッサ24個の環境で3Dプリンター用スライサーを実行した測定では、受信スレッドの
base priority 8 でスライス1回あたり失敗4回と受信停止、10 で失敗1回、13 で0回でした。
停止した区間では write 区間の経過が52〜87msに対し消費CPUは31〜47usで、処理量ではなく
実行待ちが原因でした。

対処として、受信スレッドを `THREAD_PRIORITY_TIME_CRITICAL` で開始し、PATゲートが開いた後の
配信書き込みを1転送分へまとめています。188バイトごとに書くとStreamBufferのロック取得が
1転送あたり最大320回になり、優先度を上げても write 区間に31.9msが残りました。
両方を適用した測定では、スライス4回で失敗0回、write 区間の最大は395us、
完了待ちの最大は12.4msで、TVTestのD・E・Sはいずれも0でした。

投入の遅れが欠落にならないバルク転送の他機種は、この引き上げの対象外です。

## 試験

### USB受信エラーの自動復旧

DriverHostはUSB転送のエラー31・87と受信タイムアウト時に、同じ配信接続を保って受信の再開を試みます。
未完了I/Oの取消・完了回収・バッファ登録解除の後、capture停止、alt0、alt1、capture開始の順に戻します。
新しい受信ではTSの端数とPATの組み立て状態を捨て、選択したTSIDのPATを再確認します。
同じチャンネルの自動復旧では配信世代とTSのCCを変更しないため、欠落した連番は配信先でも観測できます。
欠落をすべてTVTestのDで検出できる保証はなく、DriverHostの復旧ログにも停止時間を残します。
復旧履歴をファイルに保存する場合は `DriverHost_PX4.ini` の `DebugLog=true` を指定してください。

故障時の無限再試行を避ける運用上の上限は、連続して実らなかった復旧の8回です。
復旧の直後からHealthyRun (5秒) を超えて受信できた場合は、それまでの連続回数を忘れます。
高負荷による一過性の失敗は復旧後に長く受信できるため、この数え方では受信全体を止めません。
実らない再試行が続く場合は、2回目から250ms刻みで最大2秒まで待機を広げ、
復旧手順の制御転送が同じブリッジを占有し続けないようにします。
待機時間と連続回数は `auto recovery begin` の `backoff_ms` と `consecutive` に記録します。
復旧操作自体の失敗、USB抜去、停止・選局要求、対象外のエラーでは自動復旧を終了します。停止・選局と復旧が競合する場合は、
受信機のロックを待つ間も停止要求を監視し、終了処理のjoinを妨げない構成です。
調査用の `pxbcud_usb_diagnostic.exe` は自動復旧を無効にし、最初の異常時の状態を保存するために使います。

Visual Studio 2022 / v143 で `winusb/build.ps1` を実行すると、通常の試験に加えて
`pxbcud_frontend_test` を x86 / x64 で実行します。実機用ツールはビルドのみで、自動実行の対象外です。

```powershell
# ドライバー未導入でも USB hub 経由で記述子を読む
.\winusb\build\x64\Release-static\pxbcud_usb_probe.exe

# WinUSB 導入後、基板とチューナーの識別・初期化だけを確認
.\winusb\build\x64\Release-static\pxbcud_live_test.exe

# IF 周波数[kHz]、TSID または slot、秒数、任意の TS 保存先
.\winusb\build\x64\Release-static\pxbcud_live_test.exe 1049480 0 60

# 10秒ごとに BS-1 -> BS-3 -> BS-1 を同じ受信機の Open 中に切り替える
.\winusb\build\x64\Release-static\pxbcud_live_test.exe 1049480 0 10 - 1087840

# 完了エラー31・87を順に模擬注入し、同じ配信接続で再選局後の受信を確認
.\winusb\build\x64\Release-static\pxbcud_live_test.exe 1049480 0 2 - --recover

# 同じチャンネルで再選局せず自動復旧することと、連続故障時の打切りを確認
.\winusb\build\x64\Release-static\pxbcud_live_test.exe 1049480 0 2 - --auto-recover
.\winusb\build\x64\Release-static\pxbcud_live_test.exe 1049480 0 2 - --auto-limit
.\winusb\build\x64\Release-static\pxbcud_live_test.exe 1049480 0 2 - --auto-stop

# 自動復旧を止め、最大1時間の最初の異常を時刻付きログへ保存する
.\winusb\build\x64\Release-static\pxbcud_usb_diagnostic.exe 1049480 0 3600
```

`--recover` は実機の正常なUSB転送完了を回収した後、試験ツール内でエラーを模擬します。
各エラーの通知と再選局後のパケット増加、同じ読取スレッドの継続、同期・TEI・CCを確認します。
配布用DriverHostには模擬注入処理を含みません。`--auto-recover` は再選局なしの復旧と
その間のCC欠落を記録し、`--auto-limit` は連続4故障に対して3回で復旧を打ち切ることを確認します。
通常受信と手動再選局の試験は従来どおりCCエラー0を成功条件とします。
自動復旧打切り後も配信接続は手動再選局を待ち、USB抜去や接続終了時は配信も終了します。
この試験は復旧経路を対象としており、実際のUSBエラー31・87の発生原因は別途調査が必要です。

調査ツールは現在の作業ディレクトリに `PX-BCUD-USB-yyyyMMdd-HHmmss.log` を作成し、
異常時に完了状態、最大8個の異常パケットStatus、直近32要求の投入・完了回収時刻を出力します。
`iso_errors=0` は完了結果を正常に取得できた転送の集計であり、失敗した転送の正常性を保証しません。

負荷時の調査では `timing start` のPID/TIDとETWのCSwitch・ReadyThread・DPC/ISRを照合できます。
要求履歴の `wait_begin_us` / `wake_us` はアプリ側の待機開始と復帰、`framing_end_us` はTS同期処理の終了、
`write_end_us` はPAT検査・バッファ書込・通知の終了、`submit_begin_us` は次の投入処理の開始です。
`framing_cycles` / `write_cycles` は各区間で受信スレッドが消費したCPUサイクルで、待機時間そのものではありません。
`timing maxima` は受信区間全体の最大値です。時刻取得・CPUサイクル計測の小さな負荷を含むため、
実行待ちによる遅延か処理自体の負荷かはETWのスケジューリング記録も合わせて確認してください。

実機ツールは別の DriverHost プロセスを起動せず、対象 PX-BCUD へ直接接続します。
稼働中の DriverHost / TVTest を停止する必要はありませんが、同じ PX-BCUD を別のアプリと
同時に使うと制御が競合するため、実機ツールを終了してから BonDriver を開いてください。

### 2026-10-28 の検証結果

Windows 11 Pro（10.0.26300）、Visual Studio v143、PX-BCUD revision 0205、
BS/CS アンテナ接続・外部給電の構成で確認しました。

| 試験 | 結果 |
|---|---|
| 全体ビルド・既存自動試験 | x86 / x64 成功 |
| PX-BCUD 故障注入・分割・PAT 試験 | x86 / x64 成功 |
| x86 直接受信、BS-1、60秒 | 配信693,267パケット、同期・TEI・CC・USBエラー0、C/N表示17.261dB |
| x64 直接受信、BS-1 -> BS-3 -> BS-1、各10秒 | 配信402,513パケット、同期・TEI・CC・USBエラー0 |
| x64 直接受信、CS110、10秒 | 配信346,685パケット、同期・TEI・CC・USBエラー0、C/N表示15.613dB |
| x64 BonDriver 経由、BS-1 / BS-3、8周期 x 3秒 | 345,088パケット、選局失敗・無入力・同期・TEI・CCエラー0 |
| TVTestでのチャンネルスキャン・視聴 | 利用者による動作確認済み |
| USBの抜き差し | 利用者による動作確認済み |

開始時の選択 PAT 検証で除外したパケットは上記の配信数に含みません。
raw 受信数と `startup_discarded_packets` はワーカーログへ別々に記録します。

## 導入と制限

INF のカタログは、既存の `winusb/pkg/signing-tools/sign.ps1` で
`winusb/pkg/inf` 内の全 INF を対象に再生成できます。
同スクリプトで CRLF / BOMなしに正規化した後、INF とカタログの一致を検証します。
通常配布は `winusb/build.ps1` で作成し、`tools/check_pxbcud_package.py` で
両ビット数、必須ファイル、機種定義と GUID、外部給電・TS 世代管理の設定を検証します。
診断用ツール付きの個別パッケージは、従来の
`winusb/pkg/pxbcud-experimental/package.ps1 -OutputPath <出力先>` でも生成できます。
どちらも同じ INF・BonDriver 設定・共通機種定義を使用します。

INF のインストールは管理者権限を要します。証明書の信頼状態は既存の配布方式に従って
確認してください。署名の検証が通らない場合は、無効化して先へ進まず原因を確認します。

実験用 DriverHost 設定を既存環境へ統合する際は、`DeviceDefinition` の連番に合わせます。
DriverHost は全体で1プロセスを共有するため、別ディレクトリの exe を置くだけでは
稼働中の旧ホストと切り替わりません。利用中の録画を終え、アプリを閉じた後に更新版へ
切り替える必要があります。

[メーカー公式FAQ](https://www.plex-net.co.jp/product/px-bcud/support.html)によると、
PX-BCUD はアンテナ給電に非対応です。外部給電と `LNBPower=0` が前提で、
0以外の給電要求は非対応エラーになります。
内蔵カードリーダーの登録はなく、復号には既存の外付けリーダーなどを利用します。
対応周波数は公開 PLL テーブルの範囲（入力 IF 950500 <= f < 2151500 kHz）で、
新4K8K衛星放送の受信対応は含まれません。
未確認の項目は、長時間録画時の受信安定性と、スリープ（サスペンド）復帰後の動作です。
公開リリースは、これらの確認と、torneの[現行統合版の実機検証](../winusb/docs/torne-integration-notes.md)が完了してから行う方針です。
