# PLEX PX-BCUD の Linux 対応の設計 (px4-cused)

この文書は、PLEX PX-BCUD (USB ID `3275:0085`) を Linux で使うための構成案です。  
PIX-DT295 ([20261120_dt295_linux_cuse_design.md](20261120_dt295_linux_cuse_design.md))、Sony torne CECH-ZD1J ([20261121_torne_linux_cuse_design.md](20261121_torne_linux_cuse_design.md))、I-O DATA GV-MVP/XZ3 ([20261121_gv_mvp_xz3_linux_cuse_design.md](20261121_gv_mvp_xz3_linux_cuse_design.md)) と同じく、ユーザー空間のデーモン `px4-cused` が libusb で機器を動かし、CUSE でデバイスファイルを公開する方式を前提にします。  
維持する契約、WinUSB 版と共有するコード、Linux で測り直す値、試験の範囲、利用者の判断を仰ぐ事項を整理します。  
Linux の動作はすべて設計上の想定で、Linux の実機では確かめていません。  
WinUSB 版での測定値は、出典の文書 ([px-bcud-winusb.md](px-bcud-winusb.md)) から引用します。

この文書の「検証環境」は、ほかの 3 機種の設計文書と同じ Raspberry Pi 4 Model B (64 ビット版の Raspberry Pi OS Lite) を指し、「x86_64 の試験機」は USB ハブを経由せずに機器をつなげる x86_64 の Proxmox VE のホスト (カーネル 6.17) を指します。

## 目的と範囲

recpt1、BonDriverProxy_Linux、Mirakurun などが、既存の機種と同じく受信のデバイスファイルを開き、PTX の ioctl で BS・110度CS を選局できるようにします。

PX-BCUD が受信できるのは ISDB-S (BS・110度CS) だけです。  
復調器 TC90532 の衛星側とチューナー QM1D1C0045 系だけを持つため、地デジ (ISDB-T) と CATV の J.83B は機器の構成上の対象外です。  
入力 IF は公開 PLL テーブルの範囲 (950,500 kHz 以上 2,151,500 kHz 未満) に限られ、新4K8K衛星放送も対象外です。

内蔵カードリーダーを持たないため、カードのデバイスファイル (`/dev/px4card/<ID>`) は作りません。  
復号には外付けのカードリーダーを pcscd から使います。

メーカーの FAQ のとおりアンテナ給電に対応しないため、WinUSB 版と同じく `PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS` を返し、外部からの給電を前提にします。

EM28178 はファームウェアを読み込まない USB ブリッジで、WinUSB 版も機器へ転送するプログラムを持ちません。  
Linux 版にも、ファームウェアの配置と埋め込みは不要です。

WinUSB 版の PX-BCUD 対応は実験的な位置付けです。  
Windows で確かめた範囲は、x86 と x64 での BS・110度CS の受信、BS のチャンネル切り替え、TVTest でのスキャンと視聴、USB の抜き差し、高負荷時の isochronous 転送の維持です。  
長時間録画とスリープからの復帰は Windows でも未確認で、Linux の結果と分けて記録します。

## カーネルの em28xx ドライバーとの関係

Linux の本体には、PX-BCUD に対応した `em28xx` ドライバー (`em28xx-dvb`、`tc90522`、`qm1d1c0042`) があります。  
検証環境 (カーネル 6.18) と x86_64 の試験機 (カーネル 6.17) のどちらでも、`modinfo -F alias em28xx` に `usb:v3275p0085` があり、機器をつなぐと `em28xx` が interface 0 を取って DVB のデバイス (`/dev/dvb/adapterN`) を作る想定です。

`em28xx` を使えば、DVB API に対応したアプリケーション (dvbv5-zap、Mirakurun の DVB 対応など) から受信できます。  
`px4-cused` で PX-BCUD を扱う理由は次のとおりです。

- 既存の px4_drv と同じ PTX の ioctl (`/dev/*videoN`) で選局でき、recpt1 などの設定を機種ごとに分けずに済む
- 選局の直後に機器の FIFO に残る前のチャンネルの TS を、選んだ TSID の PAT を確かめるまで配信しない (WinUSB 版の `PatGate`)
- isochronous 転送の失敗から、同じチャンネルのまま受信を再開する (WinUSB 版の自動復旧と `RecoveryBudget`)
- チューナーの全レジスターの初期化と、TS の出力ピンの設定が、WinUSB 版の実機で確かめた手順のまま使える

`em28xx` の PX-BCUD の受信品質は、この文書の範囲では確かめていません。  
2 つのドライバーは同じ interface を取り合うため、`px4-cused` は機器を開くときに `em28xx` を外し、閉じるときに戻します (「決めた事項」の 2)。

USB の interface をカーネルのドライバーから外す操作 (`libusb_detach_kernel_driver`、usbfs の `USBDEVFS_DISCONNECT`) は、rtl-sdr のツールが一般のユーザーで `dvb_usb_rtl28xxu` を外す例と同じく、USB 機器のファイルを開ける権限で行える想定です。  
`px4-cused.service` は capability を持たないため、この想定は x86_64 の試験機で確かめます。

## ほかの機種との違い

| 項目 | GV-MVP/XZ3 | PX-BCUD |
|---|---|---|
| USB ブリッジ | boot0 の RAM の一部を、ホストが中継用のプログラムで一時的に置き換える | EM28178 (ファームウェアなし) |
| 制御転送 | vendor request `0xBC` のメモリ要求 | vendor request 0 (ブリッジのレジスター)、2・3 (I²C) |
| 復調器とチューナー | TC90522 系 | TC90532 (衛星側だけ、I²C `0x15`)、QM1D1C0045 系 (`0xfe` 経由で `0x61`) |
| 受信できる放送 | 地デジ・BS・110度CS | BS・110度CS |
| TS の取り出し | 制御転送の「操作12」で Bulk IN へ送り出させる | alternate setting 1 の isochronous IN `0x84` (940 バイト × 1 マイクロフレームごと) |
| TS の形式 | 192 バイトのレコード | 通常の 188 バイトの TS (同期バイト `0x47`)。isochronous のパケットの境界と TS の境界は一致しない |
| カード | 内蔵カードリーダー | なし |
| アンテナ給電 | 非対応 | 非対応 |

PX-BCUD の特徴は、既存の 3 機種がすべて Bulk IN を使うのに対し、isochronous 転送で TS を受け取る点です。  
isochronous 転送は、ホストが要求を出すのが遅れると、そのマイクロフレームのデータを取り返せません。  
Bulk IN の機種では受信の遅れが USB の流量制御で吸収されますが、PX-BCUD では TS の欠落になります。

## 構成

`px4-cused` に 4 つ目の機種として `PxBcudDevice` を追加します。  
`Device` 基底クラス、ホットプラグ、受信のリングと PTX の ioctl (`video_node`) は、ほかの機種と同じものを使います。  
カードのデバイスファイルを作らないため、`InitModel()` は `card_id_` を設定しません。

| デバイスファイル | 内容 |
|---|---|
| `/dev/pxbcudvideoN` (「決めた事項」の 1) | 受信。PTX の ioctl、`read`、`poll` |

### 機種ごとの処理の境界

| 層 | 共有するもの | Linux 版だけのもの |
|---|---|---|
| ブリッジ、復調器、チューナーの制御 (`pxbcud::Frontend`) | WinUSB 版の `frontend.cpp` をそのまま使う | `pxbcud::Io` の実装 (libusb の同期制御転送と、抜去で直ちに戻る待機) |
| TS の境界の獲得 (`PlainFramer`) | WinUSB 版の `plain_framer.hpp` をそのまま使う | なし |
| 前のチャンネルの TS の除外 (`PatGate`) | WinUSB 版の `pat_gate.hpp` をそのまま使う | なし |
| 自動復旧の回数と待機 (`RecoveryBudget`) | WinUSB 版の `recovery_budget.hpp` をそのまま使う | libusb の失敗の種類から「再試行できる失敗」を決める対応表 |
| isochronous 転送の発行と回収 | なし | libusb の非同期 isochronous 転送、alternate setting の切り替え |
| C/N | `Frontend::ReadCn` (TC90532 の `0xbc`〜`0xbd`) | PTX の `GET_CNR` へ生の値を返す (変換は recpt1 などの利用者側) |

## WinUSB 版と共有するコード

WinUSB 版の [`pxbcud/`](../winusb/src/DriverHost_PX4/pxbcud/) のうち、`frontend.{hpp,cpp}`、`plain_framer.hpp`、`pat_gate.hpp`、`recovery_budget.hpp` は Windows の API を使っていません。  
`Frontend` は USB の制御転送と待機を `pxbcud::Io` のインターフェースで受け取り、既存の `pxbcud_frontend_test` も模擬の `Io` で動いています。  
この 4 つは変更せずに Linux 版から使えます。

Windows の API と結び付いているのは [`pxbcud/device.cpp`](../winusb/src/DriverHost_PX4/pxbcud/device.cpp) の `Receiver` で、次の処理を含みます。

- `WinUsb_RegisterIsochBuffer` と `WinUsb_ReadIsochPipeAsap` による isochronous 転送の発行と、`OVERLAPPED` による回収
- 受信スレッドを `THREAD_PRIORITY_TIME_CRITICAL` へ上げる処理と、`QueryThreadCycleTime` による区間ごとの CPU サイクルの記録
- Win32 のエラー番号 (31、87、タイムアウト) で自動復旧の対象を決める処理
- `StreamBuffer` の epoch と BonDriver の `FramedStream` による、選局をまたぐ配信の世代管理

このうち、isochronous 転送の発行の仕組みは OS ごとに大きく異なります。  
WinUSB 版は受信スレッドが完了を待って次の要求を出すため、スレッドが走れない時間がそのまま欠落につながり、要求の数 (既定 10 本、80 ms 分) と優先度の引き上げで補っています。  
libusb では、カーネルの `em28xx` と同じく完了の通知 (コールバック) の中で同じ転送を出し直せるため、待機の構造が WinUSB 版と一致しません。

「決めた事項」の 3 のとおり、OS に依存しない 4 つのファイルだけを共有し、`Receiver` の受信ループは切り出さずに Linux 版で書きます。  
WinUSB 版のコードを変えないため、Windows の動作は変わらず、Windows の回帰試験は既存の自動試験 (`pxbcud_frontend_test`) の確認に限られます。  
受信ループを共有のクラスへ切り出す案では、WinUSB 版の受信の測定値 (高負荷時の失敗 0 回など) を切り出しの後に Windows の実機で取り直す必要があります。

### ライセンス

`frontend.cpp` は knight-rider/ptx と Linux の `em28xx`、`qm1d1c0042` の手順を参考にした GPL-2.0 のコードで、出典を冒頭のコメントに記載しています。  
`px4-cused` も GPL-2.0-only のため、同じファイルをそのまま組み込めます。

## PX-BCUD 固有の扱い

### 開く処理と初期化

WinUSB 版は、受信機の `Open` で `Frontend::Initialize()` (ブリッジの GPIO と I²C の設定、復調器の初期化、チューナーのソフトリセットと全レジスターの書き込み) を行い、`Close` で `Sleep()` を呼びます。  
Linux 版も `VideoOpen` と `VideoClose` で同じ処理を呼びます。  
制御の期限は WinUSB 版の値 (初期化 8 秒、選局 5 秒、TSID の選択 2 秒、そのほか 1 秒) を、操作全体の絶対期限としてそのまま使います。

### 選局と TSID

`PTX_SET_CHANNEL` の `freq_no` と `slot` は、ほかの機種と同じ `channel.hpp` の `ChannelToTune()` で周波数と TSID (または相対 TS 番号) に換算します。  
`Frontend::SelectStream()` は、8 未満の値を相対 TS 番号として TMCC (`0xce` 以降) から TSID を求めるため、GV-MVP/XZ3 で追加したような変換は不要です。  
ISDB-T の要求には `AcceptsSystem()` が false を返し、`PTX_SET_SYSTEM_MODE` で地デジを選んだ利用者には `EINVAL` を返します。

WinUSB 版と同じく、選局の前に受信を止めて未完了の転送をすべて回収し、TS の端数と PAT の組み立ての状態を捨てます。  
`VideoNode` は `StartStream` の前にリングを空にするため、選局をまたいで前のチャンネルの TS が利用者へ届く経路は、機器の FIFO に残る TS だけです。  
これは WinUSB 版と同じく `PatGate` で除外し、除外した数を `startup_discarded_packets` として記録します。

### isochronous 転送

WinUSB 版の実機 (revision `0205`) では、interface 0 の alternate setting 1 に isochronous IN `0x84` (最大 940 バイト、interval 1) があり、alternate setting 0 の同じ endpoint は最大長 0 でした。  
Linux 版も受信の開始で記述子を照合し、一致しない場合は `ENOSYS` で受信を断ります。

受信の開始と停止の順序は WinUSB 版と同じです。

1. 開始: `libusb_set_interface_alt_setting(handle, 0, 1)`、`Frontend::Capture(true)`、転送の発行
2. 停止: すべての転送の取り消しと完了の回収、`Frontend::Capture(false)` (ブリッジを先に止めて出力ピンを戻す)、alternate setting 0

転送が5秒以内に回収できない場合、バッファは遅延した完了通知まで保持し、USB の挿し直しまでその個体での再開を止めます。

1 本の転送は WinUSB 版と同じ 64 マイクロフレーム (8 ms、60,160 バイト) とし、本数は WinUSB 版の既定と同じ 10 本 (80 ms 分) から始めます。  
`libusb_alloc_transfer(64)` と `libusb_fill_iso_transfer` で作り、完了のコールバックの中で各パケットの `status` と `actual_length` を確かめて `PlainFramer` と `PatGate` へ渡し、同じ転送を出し直します。  
コールバックは `main.cpp` の libusb のイベントのスレッドで動き、ほかの機種の Bulk IN の完了と同じスレッドを共有します。  
コールバックの中の処理は TS の切り出しと `VideoNode::Push()` (リングへの複写) に限り、ロックを長く保持する処理や制御転送を行いません。

パケットの `status` の異常と長さの異常は、WinUSB 版と同じく `PlainFramer::Gap()` で端数と同期を捨て、`iso_errors` として数えます。  
受信が 5 秒途切れた場合 (`no_data`) と、5 秒以内に選んだ TSID の PAT が届かない場合 (`pat_timeout`) の扱いも WinUSB 版と同じです。

### 自動復旧

WinUSB 版は、転送の失敗 (Win32 のエラー 31 と 87) と 5 秒の無受信で、同じ配信の接続を保ったまま、取り消しと回収、capture の停止、alternate setting 0、1、capture の開始の順に受信を再開します。  
連続して実らなかった復旧の 8 回で打ち切り、5 秒を超えて受信できた後は回数を数え直し、データが 1 度も届かない無受信の再試行は回数に数えません (`RecoveryBudget`)。

Linux 版も同じ手順と同じ `RecoveryBudget` を使います。  
libusb で再試行の対象にする失敗は、転送全体の `LIBUSB_TRANSFER_ERROR` と、出し直し (`libusb_submit_transfer`) の `LIBUSB_ERROR_IO` を想定し、`LIBUSB_TRANSFER_NO_DEVICE` と `LIBUSB_ERROR_NO_DEVICE` は抜去として復旧を終えます。  
Windows のエラー 31・87 が Linux のどの失敗に当たるかは分かっていないため、x86_64 の試験機と検証環境で起きた失敗の種類を記録してから対応表を見直します。  
復旧の処理は制御転送と待機を伴うため、libusb のイベントのスレッドでは行わず、機器ごとの受信のスレッドへ通知して行います。

### Linux で測り直す値

次の値は Windows の実測から決めたもので、Linux では完了の通知の中で転送を出し直すため、必要な値が変わる可能性があります。

| 値 | WinUSB 版 | 根拠 |
|---|---|---|
| 転送の本数 | 10 本 (設定 `MaxUrbs`、2〜32) | 5 本では完了待ちの最大が 33〜42 ms で、余裕の 40 ms に並んだ |
| 受信スレッドの優先度 | `THREAD_PRIORITY_TIME_CRITICAL` | 全論理プロセッサを占有する負荷で、base 8 では失敗 4 回と受信停止、base 13 で 0 回 |
| 配信の書き込みの単位 | 1 転送分をまとめて 1 回 | 188 バイトごとでは 1 転送あたり最大 320 回のロックの取得で、書き込みの区間が 31.9 ms に延びた |

Linux 版は、転送の本数を 10 本、優先度を既定 (`SCHED_OTHER`) のまま、書き込みを 1 転送分にまとめた形で始めます。  
完了の通知の間隔の最大と、出し直しまでの時間の最大を受信の終了時にログへ記録し、CPU を占有する負荷 (`stress-ng --cpu` など) をかけた状態で isochronous の失敗が起きるかを測ります。  
失敗が起きる場合は、本数を増やすか、libusb のイベントのスレッドの優先度を上げるかを測定値から決めます。  
優先度を上げるには systemd のユニットの設定 (`CPUSchedulingPolicy` など) を変える必要があり、ほかの機種の受信にも影響するため、測定の前には変えません。

### USB の接続経路

isochronous 転送は、ハブを経由すると Transaction Translator の帯域の予約を伴う場合があり、Bulk IN の機種より経路の影響を受けやすい転送です。  
検証環境の USB ポートは本体内の VL805 の内蔵ハブを経由するため、最初の実機試験は USB ハブを経由しない x86_64 の試験機で行い、その後に検証環境で同じ試験を繰り返します。  
検証環境で欠落が起きる場合は、検証環境では推奨しない構成として記録します。

## デバイスファイルの契約

受信はほかの機種と同じ PTX の ioctl の契約です。

| ioctl | PX-BCUD の扱い |
|---|---|
| `PTX_SET_SYSTEM_MODE` | ISDB-S だけを受け付ける |
| `PTX_SET_CHANNEL` | 衛星の `freq_no` 0〜23。`slot` は 8 未満を相対 TS 番号、それ以上を TSID として扱う |
| `PTX_START_STREAMING`、`PTX_STOP_STREAMING` | isochronous 転送の開始と停止。開始は最初の TS を待たずに戻る |
| `PTX_GET_CNR` | TC90532 の `0xbc`〜`0xbd` の生の値 (ほかの機種の ISDB-S と同じ値の意味) |
| `PTX_ENABLE_LNB_POWER` | 0 は成功、0 以外は `ENOSYS` |

受信機は 1 つのため、開けるのは同時に 1 つの利用者だけです。  
自動復旧を打ち切った後と、抜去の後の `read` は、ほかの機種と同じく `EIO` を返します。  
同じチャンネルの自動復旧の間は配信を止めず、欠落は continuity counter の不連続として利用者側で観測できる状態のまま残します。

## 機器の着脱とデーモンの寿命

USB の抜去時の扱いはほかの機種と同じで、開いている利用者がいる間はデバイスファイルを残し、受信は `EIO` を返します。  
抜去を検出したら、未完了の転送の回収を待ってから `Device` を破棄します。  
isochronous の転送は抜去で `LIBUSB_TRANSFER_NO_DEVICE` として完了するため、回収を待つ間に制御転送を行いません。

デーモンの終了の手順 (`OnShutdown`) では、受信を止めてから `Frontend::Sleep()` を呼び、alternate setting 0 へ戻します。  
`em28xx` を外して機器を開いた場合は、機器を閉じるときに `libusb_attach_kernel_driver` で戻します (「決めた事項」の 2)。

## 試験

### 自動試験

- `make -C cuse check` に、WinUSB 版の `pxbcud_frontend_test` (初期化の要求列、短い転送と I²C の不応答の区別、TSID の選択、`PlainFramer`、`PatGate`、`RecoveryBudget`) を加える
- `PTX_SET_CHANNEL` の衛星の換算と、ISDB-T の拒否を `channel_test` で確かめる
- isochronous のパケットの処理 (異常なパケットの後の同期の破棄、長さの異常、パケットの境界をまたぐ TS) を、模擬の転送の完了で確かめる試験を加える

### 実機試験 (x86_64 の試験機、続いて検証環境)

1. 接続、`em28xx` との関係 (「決めた事項」の 2)、開く処理、チューナーの識別値 `0x68`
2. BS・110度CS の各チャンネルを 30 秒受信し、パケット数、同期・TEI・CC の異常、`iso_errors`、完了の間隔の最大を記録する
3. BS の相対 TS 番号と TSID の両方での選局、`channel_switch_stress` に相当する同じ受信中の切り替え (前のチャンネルの TS が混ざらないこと)
4. CPU を占有する負荷をかけた状態の受信
5. デーモンの再起動、受信中と待機中の USB の抜き差し
6. BS と 110度CS の長時間受信

### Windows 版の回帰試験

「決めた事項」の 3 のとおり WinUSB 版のコードを変えないため、`winusb/build.ps1` で `pxbcud_frontend_test` を x86・x64 で実行することだけを確認します。  
受信ループを切り出す場合は、[px-bcud-winusb.md](px-bcud-winusb.md) の検証 (x86 と x64 の直接受信、BS-1・BS-3 の切り替え、110度CS、BonDriver 経由の切り替え、自動復旧の模擬注入、高負荷時の受信) を切り出しの前後で比べます。

## 実装

`cuse/src/pxbcud_device.{hpp,cpp}` の `PxBcudDevice` が、`Device` の受信の各メソッドを WinUSB 版の `pxbcud::Frontend` へつなぎます。  
`pxbcud::Io` は libusb の同期制御転送で実装し、期限と抜去の扱いは WinUSB 版の `UsbIo` と同じです。  
isochronous のパケットから TS を組み立てる処理は、模擬の転送で試験できるよう `cuse/src/pxbcud_stream.hpp` の `PxBcudStream` (`PlainFramer` と `PatGate` を 1 転送ずつ呼ぶ形) に分けました。

選局は、カーネルの `ptx_chrdev_tune()` と同じくロックを最大 3 秒待ってから TSID を選びます。  
ロックしない場合と、TMCC にない相対 TS 番号や切り替わらない TSID は `EAGAIN` とし、利用者が選び直せる失敗として扱います。  
受信中の選局はほかの機種と同じく `EBUSY` を返すため、選局をまたぐ配信の世代管理 (WinUSB 版の epoch) は不要です。

`main.cpp` の機種の表に、カーネルのドライバーを外す機種の印を加えました。  
PX-BCUD だけが印を持ち、機器を開くときに `libusb_detach_kernel_driver`、閉じるとき (デーモンの終了を含む。抜去の後は除く) に `libusb_attach_kernel_driver` を呼びます。

WinUSB 版のコードは、`recovery_budget.hpp` の 1 行だけを変えました。  
`Backoff()` の比較で、積の表現型 (Linux の GCC では `long`) と `std::chrono::milliseconds` の表現型 (`long long`) が異なり、Linux ではコンパイルできなかったためです。  
変数の型を `std::chrono::milliseconds` に揃えただけで、MSVC では同じ型のため動作は変わりません。

### ビルドと自動試験

- Linux 版: x86_64 の試験機 (Debian 13 の GCC) で `make -C cuse` に新たな警告はなく、`make -C cuse check` は `pxbcud_frontend_test` と `pxbcud_stream_test` を含めてすべて成功しました
- WinUSB 版: `winusb/build.ps1` で x86・x64 のビルドと自動試験 (`pxbcud_frontend_test` を含む) が成功しました

## Linux 版の実機確認

x86_64 の試験機の USB ポートへ PX-BCUD を直接つなぎ、外部から LNB へ給電した BS・110度CS のアンテナで確かめました。  
デーモンは、`px4-cused.service` と同じ制限 (`CapabilityBoundingSet=` 空、`NoNewPrivileges=yes`、`DevicePolicy=closed` と `/dev/cuse`・USB 機器だけの許可) を `systemd-run` で付けて動かしました。

| 試験 | 結果 |
|---|---|
| 接続 | `em28xx` が機器を取って `/dev/dvb/adapter0` を作った後、デーモンが `em28xx` を外して `/dev/pxbcudvideo0` を作った。capability なしで外せた。チューナーの識別値 `0x68` |
| recpt1 で BS01_0 を 30 秒 | 341,463 パケット、同期・TEI・CC の異常 0、C/N 表示 13.6 dB。前の TS として除外したパケット 981、`iso_errors` 0、完了の間隔の最大 8.2 ms |
| 1 回開いたまま BS01 (相対 TS 番号 0)、BS03、BS01 (TSID 16400)、110度CS の 2 波を 10 秒ずつ 3 周期 | 15 区間で 3,283,546 パケット、同期・TEI・CC の異常 0。どの区間も最初のパケットは選んだ TSID の PAT で、別の TSID の PAT はなかった。選局 179〜217 ms |
| BS03_0 の受信中にデーモンを停止 | 1.5 秒で停止し、停止までの TS は異常 0。`em28xx` が戻り `/dev/dvb/adapter0` が再び作られた |
| BS01_0 の受信中に USB を抜き、約 10 秒後に同じポートへ挿し直す | 抜去までの 1,274,702 パケットは異常 0。転送の失敗 (`LIBUSB_TRANSFER_NO_DEVICE`) で受信を終え、`read` は `EIO`。挿し直した機器は `em28xx` から外して `/dev/pxbcudvideo1` で提供し (古いデバイスファイルを recpt1 が開いていたため)、recpt1 が閉じた後に古いデバイスファイルは消えた。挿し直した機器で BS03_0 を 10 秒受信し、異常 0 |

挿し直した後の同じデーモンで、110度CS の CS4 を recpt1 で 1 時間 (3,600 秒) 受信し、TS を保存せずに 1 分ごとの計数を記録しました。

| 項目 | 結果 |
|---|---|
| パケット | 124,839,676 (平均 51.5 Mbps) |
| 同期・TEI・CC・形式の異常 | すべて 0 |
| デーモンの `iso_errors`、リングの溢れ、自動復旧 | すべて 0 |
| 転送の完了の間隔の最大 | 10.5 ms |
| recpt1 の読み出しの間隔の最大 | 11 ms |

受信を終えた後にデーモンを止めると、`em28xx` が戻り `/dev/dvb/adapter0` が再び作られました。

続けて、BS の BS01_0 を recpt1 で 2 時間 (7,200 秒) 受信しました。

| 項目 | 結果 |
|---|---|
| パケット | 83,241,985 (平均 17.0 Mbps) |
| 同期・TEI・CC・形式の異常 | すべて 0 |
| デーモンの `iso_errors`、リングの溢れ、自動復旧 | すべて 0 |
| 転送の完了の間隔の最大 | 26.7 ms |
| recpt1 の読み出しの間隔の最大 | 59 ms |

読み出しの間隔が 15 ms を超えたのは、開始から約 77〜78 分の 2 分間 (最大 59 ms と 46 ms) と、約 93 分の 1 分間 (16 ms) だけでした。  
転送の完了の間隔の最大 26.7 ms は、転送の本数で吸収できる 80 ms の範囲に収まり、TS は欠けていません。  
x86_64 の試験機はほかの仮想マシンを動かしており、この時間に遅れが生じた原因は調べていません。

自動復旧の経路を実機で動かすため、CS4 の 240 秒の受信中に、利用者がアンテナのケーブルを約 30 秒抜いてから戻しました。  
TS が届かなくなる (`no_data`) と想定していましたが、抜いている間も TS は同じ流量 (約 52 Mbps) で届き続け、TEI の立ったパケットとして配信されました。  
そのため自動復旧は始まらず、アンテナを戻した後は同じ受信のまま TEI が止まりました。

| 項目 | 結果 |
|---|---|
| パケット | 8,298,560 (TEI 263,711、CC の飛び 24、形式の異常 2) |
| TEI が記録された区間 | 開始後 50〜70 秒の 10 秒ごとの集計の中だけ。以降の約 170 秒は TEI・CC の異常 0 |
| デーモン | `iso_errors` 0、リングの溢れ 0、自動復旧 0 回。デーモンの TEI の計数は recpt1 側と同じ 263,711 |

TEI の立ったパケットの数は約 7.6 秒分の流量で、抜いていた約 30 秒より短い理由は分かっていません。  
アンテナの断線では受信が止まらないため、自動復旧の経路は模擬の故障で確かめました。

### 模擬の故障による自動復旧の確認

WinUSB 版の `pxbcud_live_test` の `--auto-recover`、`--auto-limit`、`--auto-nodata` に当たる故障を、`make -C cuse fault` で作る `build/px4-cused-fault` に入れました。  
故障の処理は `PX4_CUSED_FAULT_INJECTION` を定義したビルドだけに含まれ、配布する `px4-cused` には含まれません (実行ファイルに環境変数の名前の文字列がないことを確かめました)。  
受信の開始時に環境変数 `PX4_CUSED_PXBCUD_FAULT` を読み、`error:回数:開始秒` は完了した転送を失敗として扱い、`nodata:秒数:開始秒` は届いたパケットの長さを 0 として扱います。

x86_64 の試験機で、`px4-cused.service` と同じ制限を付けたデーモンで 110度CS の CS4 を受信しました。

| 故障 | 結果 |
|---|---|
| `error:3:10` (40 秒) | 3 回とも同じチャンネルのまま再開した。待機は 0、250、500 ms で、WinUSB 版と同じ。最後まで TS が届き (1,333,364 パケット)、`read` は失敗しなかった。読み出しの間隔の最大 523 ms と CC の飛び 69 は故障による欠落 |
| `error:9:10` (40 秒) | 8 回の復旧の後、9 回目の故障で `auto recovery exhausted limit=8` となり受信を終えた。以後は TS が届かない |
| `nodata:55:10` (90 秒) | 5 秒ごとに `no_data` で 10 回再開し、上限の 8 回を超えても打ち切らなかった。データの 1 度も届かない再試行は連続の回数に数えず (`consecutive=1`、`no_data=1`)、待機もなかった。故障を止めた後は同じ受信のまま 52 Mbps に戻った |

recpt1 は `read` の失敗を無視して録画の時間まで読み続けるため、抜去の後も指定の時間まで終わりませんでした。  
カーネルモジュールの機種でも同じ動作です。

### 検証環境 (Raspberry Pi 4) での受信

PX-BCUD を検証環境の本体の USB ポート (VL805 の内蔵ハブを経由) へつなぎ、`make install` で導入した `px4-cused.service` で確かめました。  
`make check` は検証環境 (aarch64) でも成功しました。  
デーモンは `em28xx` を外して `/dev/pxbcudvideo0` を作り、udev のルールにより `video` グループ、0664 になりました。

| 試験 | 結果 |
|---|---|
| recpt1 で BS01_0 と CS4 を 30 秒ずつ | 340,090 と 1,037,877 パケット、同期・TEI・CC の異常 0、`iso_errors` 0、転送の完了の間隔の最大 8.2 ms |
| 1 回開いたまま 5 チャンネルを 10 秒ずつ切り替え (15 区間 × 3 回、30 区間 × 1 回) | 75 区間のうち 74 区間は異常 0 で、別の TSID の PAT もなかった。最初の 15 区間で 1 区間が失敗と判定されたが、出力の最後の数行しか残しておらず、区間と項目は分からない。同じ時間のデーモンのログに転送の失敗、TEI、自動復旧はない |
| recpt1 で CS4 を 2 時間 (7,200 秒) | 249,719,282 パケット (平均 50.9 Mbps)、同期・TEI・CC・形式の異常 0、`iso_errors`・リングの溢れ・自動復旧 0、読み出しの間隔の最大 9 ms、転送の完了の間隔の最大 9.0 ms |

検証環境の VL805 の内蔵ハブを経由しても、isochronous 転送の欠落は起きませんでした。

## 決めた事項

利用者の判断を仰いだ事項と、その結果は次のとおりです。

| 事項 | 選択肢 | 設計者の推奨 | 決定 |
|---|---|---|---|
| 1. 受信のデバイスファイルの名前 | `pxbcudvideoN` / `bcudvideoN` | `pxbcudvideoN` (WinUSB 版の名前空間 `pxbcud` と設定の `Type="PXBCUD"` と一致する) | `pxbcudvideoN` (2026-11-22) |
| 2. カーネルの `em28xx` との関係 | A. `px4-cused` が機器を開くときに `em28xx` を外し、閉じるときに戻す / B. 外さず、利用者が `em28xx` を外す手順を README に記載する | A (`em28xx` は既定で読み込まれ、外さないと PX-BCUD を開けないため) | A (2026-11-22) |
| 3. WinUSB 版との共有の範囲 | A. OS に依存しない 4 つのファイルだけを共有し、受信ループは Linux 版で書く / B. 受信ループも共有のクラスへ切り出し、Windows の回帰試験を済ませてから Linux 版を作る | A (isochronous の発行の構造が OS ごとに異なり、WinUSB 版の高負荷時の測定を取り直さずに済む) | A (2026-11-22) |
| 4. 最初の実機試験の環境 | x86_64 の試験機 / 検証環境 | x86_64 の試験機 (ハブを経由しない) | x86_64 の試験機 (2026-11-22) |

設計者が決めた次の事項は、異論があれば見直します。

- 受信できる放送は機器の構成どおり BS・110度CS に限り、地デジ、J.83B、新4K8K衛星放送は対象外
- アンテナ給電は WinUSB 版と同じく非対応 (`ENOSYS`)
- ファームウェアの配置と埋め込みは不要
- 転送の本数は 10 本、優先度は既定のまま始め、Linux の実測から見直す

## 未確認の条件

「Linux 版の実機確認」で、capability を持たないデーモンが `em28xx` を外せることと、抜去で起きる転送の失敗の種類は確かめました。  
次の条件は未確認です。

- `em28xx` の PX-BCUD の受信品質 (この対応では比べない)
- 抜去以外の isochronous 転送の失敗が実際に起きる条件と、Windows のエラー 31・87 との対応 (自動復旧の経路は模擬の故障で確かめた。アンテナの断線では起きない)
- CPU を占有する負荷をかけた状態の受信 (x86_64 の試験機はほかの仮想マシンを動かしているため、負荷の試験を見送った)
- 2 時間を超える受信
- 検証環境の切り替えの試験で 1 回だけ失敗と判定された区間の原因
- 高負荷時に必要な転送の本数と優先度
- 長時間録画とスリープからの復帰 (Windows でも未確認)
