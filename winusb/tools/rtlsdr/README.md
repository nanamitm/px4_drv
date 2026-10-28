# RTL2838UHIDIR の WinUSB 導入と I/Q 収録

このツールは Windows 上の `USB\VID_0BDA&PID_2838&MI_00` を WinUSB へ接続し、
RTL-SDR の個体情報・チューナー・利得一覧の取得と、有限長 I/Q データの保存を行う。
RTL2838UHIDIR は USB 側の名称であり、チューナーチップは開いた機器から判定する。
この段階の出力は復調前の I/Q で、BonDriver の TS 受信機能とは独立している。

## 構成

- `pkg/inf/RTL2838UHIDIR.inf`: 受信用 Interface 0 のみを Windows 標準 `winusb.sys` に接続。
- `rtlsdr_probe.cpp`: シリアル番号による個体選択、チューナー診断、CU8 収録。
- `Invoke-RtlSdr.ps1`: PnP 状態表示、診断プロセスの期限監視、収録結果の検証。
- `build.ps1`: x86/x64 ビルドと、SHA-256 を固定した上流 DLL の取得。
- `Install-Driver.ps1`: 署名カタログを検証して対象 INF を導入し、WinUSB の有効化を確認。

USB 複合親デバイスの `usbccgp` と、赤外線側の Interface 1 は既存の状態を維持する。
Interface 1 が未導入のままエラーコード28を示しても、I/Q 収録の成否は Interface 0 で判断できる。
既存機種の DriverHost_PX4、BonDriver、Linux カーネルドライバーを変更する必要はない。

## ビルド

PowerShell でリポジトリの `winusb` を作業ディレクトリにする。
Visual Studio の C++ ビルドツールと Windows SDK、インターネット接続が必要。

```powershell
.\tools\rtlsdr\build.ps1
# v145 のみ導入された環境では明示指定できる。
.\tools\rtlsdr\build.ps1 -PlatformToolset v145
```

実行ファイルと DLL は `build/<x86|x64>/Release-static/rtlsdr/` に配置される。
DLL は RTL-SDR Blog の V1.4.0 リリースから取得し、URL とハッシュを `runtime.json` に保存する。
通常版 librtlsdr による I/Q 収録であり、BonDriver-RTLSDR 独自の FC0013/ISDB-T パッチは含まれない。

第三者ランタイムの入手先と対応ソースは以下を参照。

- [RTL-SDR Blog V1.4.0](https://github.com/rtlsdrblog/rtl-sdr-blog/releases/tag/V1.4.0)
- [対応ソースと COPYING](https://github.com/rtlsdrblog/rtl-sdr-blog/tree/V1.4.0)

`build.ps1` は上流の ZIP を利用者の環境へ取得する。第三者 DLL は Git の管理対象外。

## 状態確認と導入

```powershell
.\tools\rtlsdr\Invoke-RtlSdr.ps1 -Action Status
```

INF を変更した場合は、先に既存のカタログ生成・検証手順を実行する。

```powershell
.\pkg\signing-tools\sign.ps1
.\pkg\signing-tools\verify.ps1 -DriverPath .\pkg\inf
```

管理者 PowerShell から導入する。

```powershell
.\tools\rtlsdr\Install-Driver.ps1
```

本リポジトリの既存配布パッケージと同じ自己署名証明書を使うため、
Windows 側でその証明書が信頼されていることが導入の前提となる。
未登録の場合は既存の `pkg/inf/cert-install.jse` の手順を参照。
導入スクリプト自体による証明書ストアの変更は不要。
署名の強制や Secure Boot の無効化を行う処理も含まれていない。

導入結果は `build/rtlsdr-install/install-result.json` と `install-driver.log` に残る。
PnPUtil がパッケージを登録しただけでは成功とせず、接続中の受信用インターフェースが
`WinUSB`・問題コード0になったことを確認する。

## 個体とチューナーの診断

```powershell
.\tools\rtlsdr\Invoke-RtlSdr.ps1 -Action List
.\tools\rtlsdr\Invoke-RtlSdr.ps1 -Action Probe -Serial '00000001'
```

シリアル番号は `List` の結果から指定する。番号が重複する機器がある場合は、
操作対象以外を取り外してから実行する。同じ番号の複数機器を自動的に1台へ絞る処理はない。
診断時には USB を開いて librtlsdr の初期化を行うため、他の SDR アプリケーションは対象機器を解放しておく。
デバイスが見つからない場合は `Status` と診断ログを併せて確認する。

## I/Q の収録

例は UHF 17ch の中心周波数を使用する。実際の受信地の物理チャンネルとアンテナに合わせて変更する。
物理チャンネル `ch` の中心周波数は `473142857 + (ch - 13) * 6000000` Hz。

```powershell
New-Item -ItemType Directory -Force .\build\iq | Out-Null
.\tools\rtlsdr\Invoke-RtlSdr.ps1 -Action Capture `
    -Serial '00000001' -FrequencyHz 497142857 -SampleRate 1015873 `
    -Seconds 30 -OutputPath "$PWD\build\iq\ch17.cu8"
```

利得を省略すると自動利得。固定値を使う場合は `-GainDb 30` のように指定すると、
チューナーが実際に対応する最も近い利得が選ばれ、適用値が結果へ記録される。

出力はヘッダーなし unsigned 8bit、I・Q 交互の CU8。
既定の約1,015,873 samples/s は `Ryujiyasu/1seg` の入力レートに合わせた値で、
30秒の要求サイズは 60,952,380 バイトとなる。
収録データをその復調器で検証するまで、放送を正常受信できたとは判定できない。

収録中は `.partial` に書き、要求サンプル数との一致を確認してから正式名へ変更する。
既存の収録ファイル、途中データ、関連ログは上書き対象外。
成功時の `.json` には周波数・要求/実サンプルレート・個体・利得・バイト数・
0/255 に達した成分数・SHA-256・ランタイムの出所が残る。
成分の飽和数は RF 信号品質や USB パケット欠落の測定値とは異なる。

失敗時は `.partial` と `.failed.json`、`.stderr.log` を調査に使える。
既定期限は要求収録時間+30秒で、`-TimeoutSeconds` で明示指定も可能。
同期 USB 読み取りが停止した場合も、PowerShell が自分で起動した子プロセスだけを終了する。
タイムアウト後は機器を開き直して次回の収録を開始する。
直接 EXE を呼ぶ場合は親スクリプトによる期限管理がないため、通常は PowerShell 側を利用する。

## 復旧・取り外し

デバイスマネージャーで受信用インターフェースのドライバーをロールバックするか、
`Status` の `Inf` に表示された該当 `oemNN.inf` を確認してから PnPUtil で削除できる。
既存の px4_drv 機種も同じ署名証明書を使うため、RTL-SDR の取り外しだけを目的に
共有証明書を削除する必要はない。

## 検証範囲

INF の対象が `0BDA:2838` の `MI_00` に限定されていること、カタログとの一致、
x86/x64 のビルド、未導入機器や誤った個体指定・出力衝突の失敗経路を確認する。
実機では機器名、チューナー、アンテナ条件、レート、周波数、収録時間とバイト数を記録する。
受信品質・長時間の USB ドロップ・選局・TS 復調は個別に確認が必要。

2026-10-28 の実機検証は Windows 11 Pro x64（10.0.26300）、MSVC v145、
シリアル `00000001`・R820T/R820T2 系の RTL2838UHIDIR で実施。
v143 は検証環境に未導入のため、既定ツールセットでのビルドは未確認。
13ch（473,142,857 Hz）・1,015,873 samples/s・利得29.7 dBで30秒収録し、
30,476,190複素サンプル・60,952,380バイトの取得と SHA-256 記録を確認した。
アンテナ条件は未確認で、I/Q の取得量だけでは正常な復調の判定はできない。
x86/x64 の2秒収録、誤った入力、既存ファイル保護、強制タイムアウトとその後の再収録を含む
`tests/rtlsdr_capture_test.ps1 -Serial '00000001'` の17項目を確認。
カタログは作業ツリーと別途チェックアウトした既存 INF の双方で検証した。

同じ30秒の保存 I/Q を、[Ryujiyasu/1seg](https://github.com/Ryujiyasu/1seg) の
コミット `5d09660def8eb45a2d603a2eaa22973ec29cb8c3` の `stream_decode` で追加検証した。
上流ソースを変更せず、7,621 / 7,634 ブロックの RS 復号に成功（99.8%）し、
1,432,748バイト・7,621パケットの TS を取得。
FFmpeg ライブラリーを使う PyAV 19.0.2 で H.264 320×180 を391フレーム、
AAC 48 kHz・ステレオを644フレーム復号できた。
出力 TS の同期エラー・transport error indicator・非 null PID の CC 飛びは0だったが、
RS 復号失敗分は上流コアが除外するため、この結果は入力全体の無欠落を保証しない。
音声復号の開始前と末尾に invalid-data エラーが各1件あり、原因の確定は未実施。
この保存 I/Q の追加検証では PAT は含まれていない。
その後の BonDriver 版では独立 helper によるライブ復調と PAT 補完を追加した。
ビルド・導入と実機確認結果は[BonDriver 統合](../../docs/rtlsdr-bondriver.md)を参照。
