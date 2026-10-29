# ISDB-C / J.83B の上流統合と移行事項

この文書は、`tsukumijima/px4_drv` の `develop`（`498beac7eb00e1210268392f77d5ae7d40e62bda`）を、このフォークの PX-BCUD・torne・RTL-SDR 対応へ統合する際の変更点をまとめたものです。受信方式の詳細は [選局仕様](isdbc_tuning.md)、上流での実測結果は [上流の検証記録](20261018_isdbc_validation.md) にあります。

## 設定の移行

同梱の `BonDriver_PX-MLT.ini` は地デジ・BS/CS を既定とし、CATV または光放送を利用する場合に `[BonDriver]` の `System` を変更します。

- ISDB-C: `System="ISDB-T,ISDB-S,ISDB-C"`
- J.83 Annex B: `System="ISDB-T,ISDB-S,J.83B"`
- 両方: `System="ISDB-T,ISDB-S,ISDB-C,J.83B"`

有効にする `System` 行を1行に絞り、他の候補は先頭の `;` でコメントにしてください。`#` や値の後ろのコメントは Windows の INI 読み込みと合わないため、説明は独立した `;` 行へ置きます。

ISDB-C は同梱の `BonDriver_PX4-C.ChSet.txt` を使い、TSMF の TS 指定は **TS0〜TS14** です。以前の相対 TS 番号・slot offset・変調方式などを含む実験用 ChSet は互換性がありません。J.83B は別ファイルの `BonDriver_PX4-J83B.ChSet.txt` を使い、中心周波数を kHz で指定します。TSMF と J.83B の指定方式が変わるため、設定・チャンネル定義を更新してから受信ソフトウェアでチャンネルスキャンをやり直してください。

旧実験用の `ExperimentalISDBC`、`ISDBCReferenceProfile`、`ForceISDBCModulation`、`ExperimentalJ83B`、`ExperimentalJ83BMonitor`、`ExperimentalJ83BBurstMonitor`、`J83BBurstWindowMs`、`J83BBurstIntervalMs`、`ExperimentalJ83BTerOverloadLevel`、`ExperimentalKeepTunerOnClose`、`ExperimentalWarmTuners`、`ExperimentalTunerRssi`、`HoldBackendPower`、`DiscardTsmfHeaderPackets` は現行の受信経路では読み込みません。旧 BonDriver の `Modulation`、`FrequencyOffsetKHz`、`InitialChannel` も上流の方式への統一に伴い対象外となりました。旧設定のままでは効果がないため、実験用設定を引き継がず新しい同梱ファイルから設定してください。

上流の実装は RF 入力の待機時整合を、ISDB-C または J.83B を初めて選局した電源投入期間に限って適用します。旧実験用の電源保持・チューナー常時動作との組み合わせを避け、上流で検証した初期化と停止の手順へ統一しています。PX-MLT8PE の `OpenPriority=-1`（3チューナー側）は維持し、5チューナー側を先に開きます。基板上の反対側デバイスの電源操作による受信誤りを、優先順位だけで防げるわけではありません。

BonDriver と DriverHost の通信パラメーターが変更されているため、DLL と EXE は同じビルドのものへ一緒に更新してください。

## macOS 用クライアントの互換更新

共有の通信ヘッダーから旧実験用パラメーターが削除されるため、`px4rec` の ISDB-C 要求も `TSMF_STREAM` 1項目にそろえました。`--tsmf` は相対 TS 番号ではなく TS0〜TS14 の指定となり、省略時は TS0 です。旧 `--modulation`・`--tsmf-slot-offset`・`--tlv` は移行が必要な指定としてエラーを返します。QAM と TLV の判定はホスト側が行い、ISDB-C ではヘッダー探索より前にキャプチャを開始します。`--b25` は MPEG-2 TS 用であり、raw TLV の変換には別の処理が必要です。

macOS 用ホストの方式文字列にも `J.83B` を追加しましたが、`px4rec` の方式指定は従来の T/S/C の範囲です。これらの macOS 変更はソースの互換更新で、ネイティブのビルドと受信の確認は別途必要です。

## このフォークで維持した処理

- PX-BCUD の USB 異常診断・有限回の自動復旧・キュー深度の設定。
- torne のネイティブフロントエンドとカードリーダー対応。
- RTL-SDR の helper 通信と、選局受付を先に返す非同期選局。
- ストリーム停止要求の保持、送信先が詰まったときの停止、選局前後のストリーム世代管理。
- DriverHost のファイルログと時刻、PX-MLT の TS 同期異常および出力バッファーの書き込み損失の計数。
- Windows のダブルクリック用 `build.cmd`、設定の同一キーの後勝ち処理、macOS 向け互換処理。

TSMF のフレームと TLV の端数は、世代を進める purge で破棄します。その際に選択済みの TS 番号の解決結果は維持します。分割 TLV は188バイト単位とは限らないため、世代付き通信のフレーム長もバイト数として検証します。長さ0・上限超過・不正ヘッダー・途中切断は引き続き失敗として扱い、通常 TS と TLV の双方で分割読み出しと世代の分離を試験します。

低レートの TSMF 出力は通常 TS の通知しきい値未満でも読み出し側を起こし、停止要求と通知の取りこぼしを防ぐ同期は両経路へ適用しています。

## 検証範囲

統合確認には Visual Studio の v143 toolset を使います。`winusb/build.ps1` は x86・x64 のソリューションと PX-BCUD、torne、ストリーム停止・世代管理、TS 同期判定、カード状態、ISDB-C/J.83B の模擬 I2C、TSMF 分離、ChSet、RTL-SDR の非同期選局・通信形式の各試験を実行します。

`stream_epoch_test` では、TSMF が内部に保持した旧フレームを世代変更で捨てること、選択済み TS を維持すること、通知しきい値未満の1パケットを届けること、読み出し開始前の停止要求を TSMF でも保持することを確認します。配布物は torne・PX-BCUD の既存チェッカーと、同梱 ChSet を読み込む試験で確認します。

今回の統合版では、次を確認しました。

- Windows: v143・x86/x64 の `build.ps1`、全組み込み自動試験、配布物生成が成功。最終ヘッダー変更後の x86 ソリューションも再ビルド。
- Windows: x64 専用の `torne_usb_probe` もビルド成功。実機への接続は行っていません。
- 配布物: `check_torne_package.py` と `check_pxbcud_package.py` が成功。ISDB-C と J.83B の両 ChSet を32bit・64bit配布に同梱。
- POSIX: WSL Ubuntu・GCC 11.4 の `macos/tests` を CMake でビルドし、`ctest` の IPC 試験が成功。
- 構文確認: WSL の GCC で `px4rec.cpp`、macOS 用 `util.cpp`、共有 `receiver_base.cpp` と `tsmf.c` を確認。macOS 固有の実行ファイルパス API と `strlcpy` は宣言だけの互換ヘッダーを使ったため、macOS ネイティブのビルド確認とは区別します。

Linux モジュールの `make -C driver -j2` は、WSL カーネル 6.18.54.1 のヘッダー配置にビルド用 Makefile がなく失敗しました。macOS 用ホスト全体の構文確認も libusb 開発ヘッダー不足で完了していません。この統合後の実機受信、長時間録画、別機種の同時受信、Linux カーネルモジュールおよび macOS ネイティブのビルドは未検証です。上流文書にある実機測定は上流環境での結果であり、この統合版の実測結果には含めません。
