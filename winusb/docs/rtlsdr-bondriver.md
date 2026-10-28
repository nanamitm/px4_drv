# RTL-SDR ワンセグの BonDriver 統合

## 接続と既存機種への影響

`BonDriver_RTLSDR.dll` は既存の BonDriver_PX4 を RTL-SDR 用設定で使用する配布名。
DriverHost_PX4 の RTLSDR デバイス実装が機器の利用権と helper の寿命を管理し、
`rtlsdr_frontend.exe` が librtlsdr による I/Q 取得と `Ryujiyasu/1seg` の連続復調を行う。
TS は既存 StreamBuffer と世代付きパイプを通り、公開 BonDriver ABI と制御パイプの形式は従来と共通。

ライブ I/Q は librtlsdr の非同期 API と既定の複数 URB で連続取得する。
復調キューは262,144バイト×128個（約16.5秒分）を上限とし、
実測約8.5秒を要した x86 の初期復号中も USB を止めずに受信する。
診断用の有限長 I/Q 収録とは異なり、ライブ入力を同期 USB 要求の再発行でつなぐ方式は使わない。

既存 DeviceBase が読み取る USB 記述子で VID/PID とシリアルを確認した後、
RTLSDR デバイスだけが WinUSB ハンドルを解放する。実際の受信中は helper が唯一の USB 所有者となる。
同一シリアルの複数機器は明示的なエラーとし、別個体を番号で推測するフォールバックは不要。
内蔵カードリーダー・LNB 給電は対象外。

## 選局と終了

Open 時の capture 要求ではバッファだけを準備し、周波数確定後に helper を起動する。
選局では旧 helper と読取 worker を停止してから StreamBuffer の世代を進め、
新しい decoder と PAT 状態を作る。初期ロックは復調後の TS 到着で確認する。
品質表示は RS 復号成功割合を使う便宜的指標で、RF C/N の dB 値とは異なる。

helper の stdin の閉鎖を停止要求に使い、終了しない場合は所有する Job Object だけを終了する。
USB 抜去時は即座に利用不能と配信停止を反映し、一覧のロック内でプロセスの終了を待つ必要はない。
helper のクラッシュ、形式不正、キューあふれ、USB エラーは失敗として記録する。
キューが満杯のときに I/Q を捨てて復調を継続する扱いは、欠落を隠すため対象外。

## helper の境界

helper は別実行ファイルとしてビルドする GPL-3.0-or-later のプログラムで、
復調依存は `Ryujiyasu/1seg` のコミット `5d09660def8eb45a2d603a2eaa22973ec29cb8c3` に固定。
DriverHost へ Rust の静的ライブラリーや復調コードをリンクする工程はない。
helper と依存元のソース、ライセンス宣言、固定リビジョンは配布用フォルダーにも保存する。

stdout は little endian の24バイトヘッダーと payload からなる。
順に magic=`0x31545352`、種別、payload 長、品質（0〜10000）、RS成功数、RS失敗数。
種別1は USB 初期化完了、2は188バイト単位の TS、3は状態、4はエラー文字列。
TS payload 上限は188×2048バイト。両端で範囲と形式を確認する。
stderr は診断ログであり、TS と混ぜずに別ファイルへ保存する。

PAT は CRC の通る SDT の TSID と固定 PMT PID の program_number を基に生成する。
既存 PAT がある場合は維持し、選局ごとに解析状態と continuity counter を初期化する。

## 試験範囲

PAT の CRC、分割 PSI、誤った pointer/adaptation、helper フレームの分割と破損、
helper の異常終了と期限、選局後の世代、Open/Close の繰り返しを確認する。
実機試験では13chの TS 同期、TEI、CC、PAT、PMT、音声・映像の復号結果を記録する。
長時間・全チャンネル・他機種との同時録画は個別の検証が必要。

## ビルドと導入

Visual Studio の C++ 開発ツールと Windows SDK、Rust/Cargo が必要。
リポジトリのルートから次を実行する。

```powershell
rustup target add i686-pc-windows-msvc x86_64-pc-windows-msvc
.\winusb\tools\rtlsdr\build-bondriver.ps1
# v143 の代わりに v145 を使う環境
.\winusb\tools\rtlsdr\build-bondriver.ps1 -PlatformToolset v145
```

`winusb/dist/BonDriver_RTLSDR_32bit` と `BonDriver_RTLSDR_64bit` に
DLL・DriverHost・helper・ランタイム・設定・ドライバー導入ファイル・helper ソースが生成される。
標準の `winusb/build.ps1` は既存機種向けのビルド経路を維持し、Rust の導入はこの専用スクリプトで必要となる。

1. 未導入の場合は配布フォルダーの `Install-Driver.ps1` を管理者 PowerShell で実行する。
   自己署名証明書が未登録の場合は既存配布と同じ `Driver/cert-install.jse` の手順を使う。
2. TVTest と同じビット数の配布フォルダーを使用し、そのファイル一式を TVTest 側へ配置する。
   `rtlsdr` サブフォルダーも必要。
3. 既存の TVTest/録画を終了して DriverHost が停止した後に新しい DriverHost へ更新する。
   古い DriverHost が既に動作中の場合、同じ通信名のため新しい RTL-SDR 定義を列挙できない。
4. TVTest で `BonDriver_RTLSDR.dll` を選択する。対象は UHF 13〜52ch の中央1セグ。
   チャンネルスキャンの待機時間は初期同期を含めて長めに設定し、必要に応じて「信号レベルを無視する」を使う。
   得られるサービスはワンセグであり、SDT に記載されたフルセグを受信する機能ではない。

`DriverHost_PX4.ini` の `DeviceDefinition17.Config` にある `GainTenthsDb` は0.1 dB単位。
実機の対応値のうち要求値に最も近い値を使用する。既定297は検証機の29.7 dB。
`TuneTimeout=20000` は同期・初期復号を待つための専用設定で、通常のハードウェア復調機より長い。
TVTest 側の BonDriver 要求タイムアウトもこれより長くする必要がある。
要求タイムアウトを指定できる TVTest では、チューナー設定で `BonDriver_RTLSDR.dll` の
要求タイムアウトを30000 msに設定する。既定の10000 msでは、実測11〜14秒の初期復調中に
「タイムアウトのためチャンネル変更ができません」となる。
`TVTest.ini` を直接編集する場合は TVTest を終了し、`[DriverSettings]` の
`DriverN_FileName=BonDriver_RTLSDR.dll` に対応する `DriverN_RequestTimeout=30000` を指定する。
未登録の場合は末尾に追加し、`DriverCount` も増やす。他の BonDriver の待機時間は変更しない。
スキャンの待機時間 (`ChannelScanWait`) とは別の設定である。
helper ログは DriverHost と同じフォルダーの `rtlsdr_frontend-<serial>.log` に残る。

## ソースと実機試験の再現

復調コアは helper 配下の `vendor/isdbt-dsp` に固定ソースを保存し、変更点を
`vendor/README.md` に記録。初期窓で出力が空になる場合の panic 回避、同期探索範囲の制限と、
連続受信中の CP タイミング・CFO 追跡を含む。
配布物の `Sources` には helper と Rust 依存を保存し、Visual Studio の開発環境から
`cargo build --offline --locked --release --manifest-path winusb/tools/rtlsdr_frontend/Cargo.toml`
で再ビルドできる。通常の helper はネットワーク接続や GNU Radio を必要としない。

既存の視聴・録画を止めずに試験する場合は、両プロジェクトを
`/p:RtlSdrIsolatedTest=true` でビルドし、
`tests/rtlsdr_bondriver_live.ps1 -IsolatedHost` を使用する。
このビルドだけが mutex/event/pipe 名に `_RTLSDR_TEST` を付け、通常版と接続を分離する。
テスト用 DLL/EXE は `build/<arch>/rtlsdr-test` に保存され、配布物は通常版の通信名を使う。

## 実機確認結果（2026-10-28）

Windows 11 Pro x64（10.0.26300）、MSVC v145、R820T/R820T2 系と判定された
RTL2838UHIDIR（シリアル `00000001`）を使用。アンテナの配線条件は未申告。
既存の TVTest/DriverHost を維持し、試験用通信名の BonDriver API クライアントで確認した。

| ビット数・周期 | 選局時間 | API受信時間 | TSパケット | PAT | 同期/TEI/CC飛び/空受信 |
|---|---:|---:|---:|---:|---|
| x86・1 | 13.578秒 | 45秒 | 14,336 | 221 | すべて0 |
| x86・2 | 12.484秒 | 45秒 | 14,304 | 221 | すべて0 |
| x64・1 | 12.687秒 | 45秒 | 14,176 | 218 | すべて0 |
| x64・2 | 11.140秒 | 45秒 | 13,952 | 214 | すべて0 |

各周期で13chへ選局し、受信後に同じ13chへ再選局して Close/Open を繰り返した。
PAT の CRC は全件有効。helper の通常停止と終了後にプロセスが残らないことを確認。
取得 TS を PyAV 19.0.2 の FFmpeg ライブラリーで解析し、
H.264 320×180 と AAC 48 kHz・ステレオを全4ファイルで実際に復号した。
音声の先頭・末尾に invalid-data が各1件あり、ファイル境界以外の原因までの確定は未実施。

PAT の4試験、DSP の33試験、helper 通信形式、既存の stream epoch・停止競合・
TS同期判定・リングバッファ purge の試験を x86/x64 で確認した。
別チャンネル間の混入検査は、両方の受信を確認できたチャンネルの指定が必要。
今回の実機確認は13chの同一チャンネル再選局に限定し、全局スキャン、TVTest GUI の表示、
数時間以上の連続録画、他機種の同時録画品質は未検証。
