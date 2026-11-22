# PLEX PX-S1UD の Linux 対応の設計 (px4-cused)

この文書は、PLEX PX-S1UD (USB ID `3275:0080`) を Linux で使うための構成案です。  
PIX-DT295 ([20261120_dt295_linux_cuse_design.md](20261120_dt295_linux_cuse_design.md))、Sony torne CECH-ZD1J ([20261121_torne_linux_cuse_design.md](20261121_torne_linux_cuse_design.md))、I-O DATA GV-MVP/XZ3 ([20261121_gv_mvp_xz3_linux_cuse_design.md](20261121_gv_mvp_xz3_linux_cuse_design.md))、PLEX PX-BCUD ([20261122_px_bcud_linux_cuse_design.md](20261122_px_bcud_linux_cuse_design.md)) と同じく、ユーザー空間のデーモン `px4-cused` が libusb で機器を動かし、CUSE でデバイスファイルを公開する方式を前提にします。  
維持する契約、WinUSB 版と共有するコード、Linux で測り直す値、試験の範囲、利用者の判断を仰ぐ事項を整理します。  
Linux の動作はすべて設計上の想定で、Linux の実機では確かめていません。  
WinUSB 版での測定値は、出典の文書 ([px-s1ud.md](../winusb/docs/px-s1ud.md)) から引用します。

この文書の「検証環境」は、ほかの 4 機種の設計文書と同じ Raspberry Pi 4 Model B (64 ビット版の Raspberry Pi OS Lite) を指し、「x86_64 の試験機」は USB ハブを経由せずに機器をつなげる x86_64 の Proxmox VE のホスト (Intel N150、4 コア、カーネル 6.17) を指します。

## 目的と範囲

Linux でスカパー！プレミアムサービス光 (ITU-T J.83 Annex B、256QAM、5.6 Msps) を PX-S1UD で受信できるようにすることが主な目的です。  
同じ機器の地デジ (ISDB-T) も、同じデーモンで扱います。

recpt1、BonDriverProxy_Linux、Mirakurun などは、既存の機種と同じく受信のデバイスファイルを開き、PTX の ioctl で地デジを選局できます。  
J.83B は、カーネルモジュールの PX-MLT 系と同じく拡張 ioctl (`PTXT_SET_PARAMS` と `PTXT_TUNE`) で選局します ([isdbc_tuning.md](isdbc_tuning.md) の「Linux の ioctl」)。

PX-S1UD の J.83B は、WinUSB 版でも実験的な位置付けです。  
チップは復調前の IQ (12 MS/s) を USB へ送り、ホストの helper が J.83B を復調します。  
Windows の 30 分の受信では、チップがブロックを落とすことと復調の作り直しによる連続性エラーが 3,655 件ありました ([px-s1ud.md](../winusb/docs/px-s1ud.md) の「J.83B の受信 (実験的)」)。  
Linux 版の目標は、この WinUSB 版と同じ品質で J.83B を受信することで、欠落そのものの改善はこの対応の範囲外です。

トランスモジュレーション方式の CATV (ISDB-C) は、WinUSB 版と同じく対象外です。  
PX-S1UD に内蔵カードリーダーはなく、カードのデバイスファイル (`/dev/px4card/<ID>`) は作りません。  
アンテナ給電の回路もないため、`PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS` を返します。

## カーネルの smsusb ドライバーとの関係

Linux の本体には Siano のドライバー (`smsusb`、`smsdvb`、`smsmdtv`) があり、x86_64 の試験機のカーネル (6.17.2-1-pve) は `3275:0080` を対応機種として持ちます。  
機器をつなぐと `smsusb` が interface 0 を取り、`/lib/firmware/isdbt_rio.inp` を読み込んで DVB のデバイス (`/dev/dvb/adapterN`) を作る想定です。  
x86_64 の試験機には、linux-firmware の `isdbt_rio.inp` が配置済みです。  
検証環境の Raspberry Pi OS のカーネル (6.18.50+rpt-rpi-v8) も `smsusb` を持ち、PX-S1UD をつなぐと読み込まれました。  
検証環境には `isdbt_rio.inp` が配置されておらず、`firmware-siano` パッケージで入れました。

両方のカーネルで、`smsusb` が機器を手放すとき (`smsusb_disconnect` から `smsusb_term_device`) に、カーネルの警告 (`free_large_kmalloc` の "Not a kmalloc allocation") とページの情報が記録されました。  
px4-cused が `smsusb` を外したときだけでなく、デーモンを止めて `smsusb` へ返した機器の USB を抜いたときにも同じ記録が出たため、`smsusb` の解放の処理によるもので、px4-cused の操作に固有のものではありません。  
記録の後もシステムと受信は動き続けました。警告の処理は解放を行わずに戻るため、手放すたびに少量のメモリーが戻らないと推定していますが、カーネルのソースでは確かめていません。

`smsdvb` は DVB-T と ISDB-T の復調結果を扱うドライバーで、J.83B を扱いません。  
J.83B は、IQ を送る専用の firmware への切り替えとホストでの復調が必要なため、カーネルのドライバーの拡張では実現できません。  
`smsusb` と `px4-cused` は同じ interface を取り合うため、PX-BCUD の `em28xx` と同じ扱いが必要です (「決めた事項」の 5)。

## ほかの機種との違い

| 項目 | PX-BCUD | PX-S1UD |
|---|---|---|
| USB ブリッジ | EM28178 (ファームウェアなし) | Siano SMS2270 (Rio)。起動のたびに firmware の読み込みが必要 |
| 制御 | vendor request の制御転送 | Bulk OUT `0x02` で Siano のメッセージを送り、応答は Bulk IN で TS と同じ経路に混ざって届く |
| 受信できる放送 | BS・110度CS | 地デジ、J.83B (実験的) |
| TS の取り出し | isochronous IN `0x84` | Bulk IN の Siano のメッセージ `MSG_SMS_DVBT_BDA_DATA` (地デジ)、`0x037C` の IQ のブロックを helper で復調した TS (J.83B) |
| firmware | 不要 | 地デジは Siano の `isdbt_rio.inp`、J.83B はこのリポジトリの `fwtool/s1ud/capture_probe.S` から作る `s1ud_j83b.inp` |
| ホストの処理量 | TS の切り出しだけ | J.83B では IQ の復調と誤り訂正 (Ryzen 9 5900X の 1 スレッドで実時間の約 0.64 倍と、別スレッドで約 0.45 倍) |
| カード | なし | なし |

PX-S1UD の特徴は、制御の応答と TS が同じ Bulk IN に届く点です。  
応答には通し番号がないため、要求と応答の対応を失った後は、その open を失敗状態にする WinUSB 版の扱いを維持します。

## 構成

`px4-cused` に 5 つ目の機種として `PxS1udDevice` を追加します。  
`Device` 基底クラス、ホットプラグ、受信のリングと PTX の ioctl (`video_node`) は、ほかの機種と同じものを使います。  
カードのデバイスファイルを作らないため、`InitModel()` は `card_id_` を設定しません。

| デバイスファイル・プロセス | 内容 |
|---|---|
| `/dev/pxs1udvideoN` (「決めた事項」の 1) | 受信。PTX の ioctl、拡張 ioctl (`PTXT_SET_PARAMS`、`PTXT_TUNE`)、`read`、`poll` |
| J.83B の helper (`msi2500_frontend serve-s1ud`) | J.83B の選局のたびに `px4-cused` が子プロセスとして起動し、標準入力で IQ のブロックを受けて、標準出力へ TS と状態を返す (「決めた事項」の 3) |

### 機種ごとの処理の境界

| 層 | 共有するもの | Linux 版だけのもの |
|---|---|---|
| Siano のメッセージの形式 (`protocol.{h,c}`) | WinUSB 版をそのまま使う | なし |
| firmware の読み込み、選局、統計、IQ の送出の要求 (`siano::Frontend`) | WinUSB 版の `frontend.cpp` をそのまま使う | `siano::Io` の実装 (Bulk OUT の送信と、受信のコールバックへ届く応答の待機) |
| 前の受信世代の TS の除外 (`CaptureGate`)、helper の TS の配信の停止 (`IqOutputGate`) | WinUSB 版の `capture_gate.hpp` をそのまま使う | なし |
| 地デジと J.83B の firmware の切り替えの順序、Bulk IN のメッセージの振り分け、受信品質のログ | 「決めた事項」の 2 による | 「決めた事項」の 2 による |
| helper との通信の形式 (`rtlsdr::Frames`) | WinUSB 版の `rtlsdr/protocol.hpp` をそのまま使う | なし |
| helper のプロセスの起動と停止 | なし | `posix_spawn`、パイプ、プロセスグループへの `SIGKILL` |
| Bulk IN の受信 | なし | libusb の非同期 Bulk 転送 |
| C/N | なし | 地デジの SNR と J.83B の MER (dB) を `DbToCndat()` で PT1/PT3 互換の値へ換算する |

## WinUSB 版と共有するコード

WinUSB 版の [`siano/`](../winusb/src/DriverHost_PX4/siano/) のうち、`protocol.{h,c}`、`frontend.{hpp,cpp}`、`capture_gate.hpp`、`firmware_path.hpp` は Windows の API を使っていません。  
`Frontend` は USB の送受信と待機を `siano::Io` のインターフェースで受け取り、既存の `siano_frontend_test` も模擬の `Io` で動いています。  
helper との通信の形式 (`rtlsdr/protocol.hpp`) と、`CaptureGate` が使う `pxbcud/plain_framer.hpp` も OS に依存しません。

Windows の API と結び付いているのは [`siano/device.cpp`](../winusb/src/DriverHost_PX4/siano/device.cpp) の `Receiver` と [`siano/iq_helper.cpp`](../winusb/src/DriverHost_PX4/siano/iq_helper.cpp) の `IqHelper` で、次の処理を含みます。

- `WinUsb_ReadPipe` による 32 本の受信要求と `OVERLAPPED` による回収、パイプのリセット
- Bulk IN のメッセージを TS、IQ のブロック、制御の応答へ振り分ける処理と、受信の開始時に前のホストの残りの転送を最大 4 回読み飛ばす処理
- `Exchange` (送信と、指定の応答の待機を 1 つの絶対期限で行う) と、失敗した後にこの open を失敗状態にする処理
- 地デジと J.83B の firmware の切り替えの順序 (J.83B の選局の前の helper の起動、J.83B 用の firmware の読み込み、閉じる前の通常の firmware への読み込み直し、ROM へ戻った場合の IN パイプのリセット)
- 統計の応答から受信品質をログへ記録する処理 (`LogSignal`)
- helper の `CreateProcessW`、job object、パイプ、5 秒の停止の期限

このうち、firmware の切り替えの順序は、WinUSB 版の実機で見つけた次の条件を含みます。

- J.83B 用の firmware は `SW_RELOAD_START` に応答せず ROM のブートローダー (モード 255) へ戻ることがあり、その際に Bulk IN の受信が一度失敗する
- J.83B 用の firmware のまま受信を止めて開き直すと `GET_VERSION` に 10 秒応答しなかったため、閉じる前に通常の firmware へ戻す
- IQ の送出を止めずに閉じると、次の open で IQ のメッセージが制御の応答と同じ Bulk IN を埋め続ける
- 前の open が失敗状態のまま閉じた場合、次の open では読み込まれている firmware を「不明」として扱う

この順序を Linux 版で書き直すと、片方の実機で見つけた条件がもう片方へ反映されない危険があります。  
GV-MVP/XZ3 と同じく、USB の転送、待機、helper のプロセス以外の処理を共有のクラス (`siano/session.{hpp,cpp}`) へ切り出してから Linux 版を作る案 (「決めた事項」の 2 の B) を推奨します。  
この案では WinUSB 版の `Receiver` の中身が変わるため、Windows の実機で地デジと J.83B の受信、地デジの切り替えを、切り出しの前後で比べます (「Windows 版の回帰試験」)。

### ライセンス

`protocol.{h,c}` と `frontend.cpp` は Khronos31/siano-userland を元にした GPL-2.0-or-later のコードで、`device.cpp` などは GPL-2.0-only です。  
`px4-cused` も GPL-2.0-only のため、同じファイルをそのまま組み込めます。

J.83B の helper (`msi2500_frontend`) は GPL-3.0-or-later で、GPL-2.0-only の `px4-cused` へは組み込めません。  
WinUSB 版と同じく別のプロセスとして起動し、標準入出力だけで通信する構成が、ライセンスの点でも障害の分離の点でも必要です。

`isdbt_rio.inp` は Siano の再配布条件のついたバイナリで、linux-firmware に含まれます。  
Linux 版は、実行ファイルへ埋め込まず、linux-firmware の配置先 (`/lib/firmware`) から読みます (「決めた事項」の 4)。  
`s1ud_j83b.inp` はこのリポジトリのソースから作る GPL-2.0-only の firmware です。

## PX-S1UD 固有の扱い

### 開く処理と firmware

WinUSB 版は、受信機の `Open` で `Frontend::Initialize()` を呼び、`GET_VERSION` のモードを見て、ROM のブートローダーならコールド起動、地デジで稼働中なら何も送らず、それ以外は firmware を読み込み直します。  
J.83B 用の firmware が稼働したままの機器 (前のホストが閉じずに終わった場合など) は、通常の firmware を読み込み直してから地デジとして使います。  
Linux 版も `VideoOpen` で同じ処理を呼び、`VideoClose` で IQ の送出を止め、J.83B 用の firmware なら通常の firmware へ戻してから受信を止めます。

firmware のファイルは次の順に探します (「決めた事項」の 4)。

| firmware | 探す場所 |
|---|---|
| 地デジ (`isdbt_rio.inp`) | `/lib/firmware/updates/isdbt_rio.inp`、`/lib/firmware/isdbt_rio.inp` (カーネルの `request_firmware()` と同じ順) |
| J.83B (`s1ud_j83b.inp`) | `px4-cused` を導入した場所の `lib/px4-cused/s1ud_j83b.inp` (既定は `/usr/local/lib/px4-cused/s1ud_j83b.inp`) |

地デジの firmware が見つからない場合も、すでに地デジで稼働中の機器 (`smsusb` が読み込んだ後など) は開けます。  
J.83B の firmware と helper が見つからない場合は、地デジの受信には影響させず、J.83B の選局だけを `ENOENT` で断ります。

### 制御の応答と TS が同じ Bulk IN に届くこと

WinUSB 版と同じく、受信を開いている間は 16 KiB の Bulk IN の転送を 32 本出し続けます。  
Linux 版は libusb の非同期転送を使い、完了のコールバックの中でメッセージを振り分けて同じ転送を出し直します。  
コールバックの中の処理は、TS の `CaptureGate` への受け渡し、IQ のブロックの helper のキューへの複写、応答の保存と待機中の `Exchange` の起床に限り、制御転送や長い待機を行いません。

`smsusb` を外した直後は、`smsusb` が取り消した転送の途中のデータが機器に残る可能性があります。  
WinUSB 版も前のホストの残りの転送を最大 4 回読み飛ばしており、Linux 版でも同じ処理が必要になる想定です。

### 選局

地デジは `PTX_SET_CHANNEL` の `freq_no` と `slot` を、ほかの機種と同じ `channel.hpp` の `ChannelToTune()` で中心周波数へ換算します。  
`Frontend::Tune()` は kHz の端数から 1/7 MHz のずれを復元するため、カーネルと同じ `93143 + freq_no × 6000` の値をそのまま渡せます。  
衛星の要求には `AcceptsSystem()` が false を返します。

J.83B は、カーネルの `ptx_chrdev_set_params()` と同じく `PTXT_SET_PARAMS` で放送方式 `PTX_J83B_SYSTEM` と中心周波数 (kHz) を受け取り、`PTXT_TUNE` で選局します。  
現在の `video_node` は拡張 ioctl に対応していないため、機種に依存しない部分として `PTXT_SET_PARAMS` と `PTXT_TUNE` を加えます。  
`PTXT_SET_PARAMS` の追加パラメーターの配列はアドレス (`prop`) で渡されるため、CUSE の `fuse_reply_ioctl_retry` で構造体を読んだ後、`num_prop` が 0 でなければ配列も読み直します。  
J.83B の追加パラメーターはないため、PX-S1UD では `num_prop` が 0 でない要求を `EINVAL` とします (カーネルと同じく `PTXT_TSMF_STREAM_PARAM` は ISDB-C だけ)。  
カーネルと同じく、J.83B を選局したデバイスファイルを閉じると放送方式は未指定へ戻します。

ロックの待機は、地デジがほかの機種と同じ最大 3 秒、J.83B が WinUSB 版の PX-S1UD と同じ最大 2 秒です。  
カーネルの J.83B は Sony の復調器に合わせて 1 秒ですが、PX-S1UD の helper は復調を始めるまでに 1 秒を超えることがあり、WinUSB 版では 1 秒の上限で選局に失敗しました。  
ロックしない場合は `EAGAIN` を返します。

受信中の選局はほかの機種と同じく `EBUSY` を返します。  
ただし、選局の前に出した Bulk IN の転送は選局の後に完了することがあるため、WinUSB 版と同じく `CaptureGate` の世代で、選局の前とロックの前の TS を配信しません。

### C/N

地デジは、統計の応答の SNR (dB) を `DbToCndat()` で、recpt1 などの地デジの式で同じ dB になる値へ換算して `PTX_GET_CNR` で返します (torne の DiB8000 と同じ扱い)。  
J.83B は、helper が報告する復調器の MER (dB) を同じく換算します。  
カーネルの ISDB-C と J.83B も地デジと同じ PT1/PT3 互換の値を返すため、利用者は方式によらず同じ式で dB を表示できます。  
MER は RF の C/N とは一致しません (WinUSB 版の 138.75 MHz で 31〜32 dB)。

### J.83B の helper

J.83B の選局では、WinUSB 版と同じ順に処理します。

1. helper を起動する。起動できない場合は firmware を切り替える前に選局を失敗させ、同じデバイスファイルで次の選局を受け付ける
2. J.83B 用の firmware を読み込む (読み込み済みなら省く)
3. IQ の送出の要求 (`0x037A`、中心周波数 Hz) を送る
4. IQ のブロック (`0x037C`) を helper の標準入力のキューへ積む
5. helper の TS を `IqOutputGate` を通して配信のリングへ書く

Linux 版の helper の起動は `posix_spawn` で、標準入力と標準出力をパイプにし、標準エラー出力はデーモンの標準エラー出力 (systemd の journal) へつなぎます。  
WinUSB 版はファイル (`px-s1ud_j83b-<機器番号>.log`) へ書きますが、`px4-cused.service` は `ProtectSystem=strict` で書き込める場所を持たないためです。  
helper は新しいプロセスグループで起動し、停止の要求 (標準入力を閉じる) から 5 秒で終わらない場合はグループへ `SIGKILL` を送ります。  
デーモンが異常終了した場合は、標準入力の EOF で helper が終わる想定です。

helper は、WinUSB 版の `msi2500_frontend` を Linux 向けにもビルドして使います。  
現在の `Cargo.toml` は `windows-sys` を全 OS の依存にしていて、MSi2500 の USB の処理 (`usb.rs`、`live.rs` の一部) も Windows 専用です。  
`windows-sys` を Windows だけの依存へ移し、Windows 専用の処理を `#[cfg(windows)]` で分けると、`serve-s1ud` は Linux でもビルドできる想定です。  
J.83B の復調 (`s1ud.rs`、`qam.rs`) には x86 と x86_64 の SIMD の処理と、それ以外の CPU 向けの処理があり、検証環境 (Cortex-A72) では後者が動きます。

### Linux で測り直す値

| 値 | WinUSB 版 | 根拠 | Linux 版の扱い |
|---|---|---|---|
| Bulk IN の転送 | 16 KiB × 32 本 | 上流の Windows 版と同じ | 同じ値で始める |
| 受信のスレッドの優先度 | `THREAD_PRIORITY_TIME_CRITICAL` | J.83B の復調が CPU を使う間、受信要求の出し直しが最大 64 ms 遅れた。チップの送信枠は約 7 ms | 既定のまま始め、IQ の受信の間隔の最大と、チップが落としたブロックの数 (IQ の停止の応答の統計) を記録して決める |
| helper の処理量 | 前処理と復調が実時間の約 0.64 倍、誤り訂正が約 0.45 倍 (Ryzen 9 5900X) | 138.750 MHz の 29 秒の収録 | x86_64 の試験機 (Intel N150) と検証環境で測る。実時間に間に合わない環境は、J.83B の対象外として記録する |
| J.83B のロックの待機 | 2 秒 | firmware を読み込み済みの選局で最大 1,125 ms | 同じ値で始める |

Linux では Bulk IN の転送がカーネルのキューに残るため、ユーザー空間の出し直しが遅れても、出してある 32 本 (約 512 KiB、IQ の約 21 ms 分) が尽きるまでは欠落しない想定です。  
この想定が成り立つかは、IQ の停止の応答にあるチップの捨てた数と、Windows の値 (30 分で 0.11%) を比べて確かめます。

Intel N150 の 1 スレッドの性能は Ryzen 9 5900X より低く、x86_64 の試験機でも helper の前処理と復調が実時間に間に合わない可能性があります。  
検証環境の Cortex-A72 では SIMD の処理がなく、さらに遅い想定です。  
helper の処理量は、実機の IQ を記録して `examples/s1ud_j83b.rs` で再生し、受信の前に測ります。

### USB の接続経路

PX-S1UD は Bulk 転送の機種で、PX-BCUD の isochronous 転送のような帯域の予約を伴いません。  
ただし J.83B の IQ は約 24 MB/s (12 MS/s × 2 バイト) と、地デジの TS (約 2.3 MB/s) の約 10 倍の流量です。  
検証環境の VL805 の内蔵ハブを経由した場合の欠落は、x86_64 の試験機と分けて記録します。

## デバイスファイルの契約

| ioctl | PX-S1UD の扱い |
|---|---|
| `PTX_SET_SYSTEM_MODE` | 地デジだけを受け付ける (カーネルと同じく J.83B は拡張 ioctl でだけ指定する) |
| `PTX_SET_CHANNEL` | 地デジの `freq_no` 3〜12、22〜62 (CATV のパススルー)、63〜112 (UHF)。`slot` は周波数の補正値 (kHz) |
| `PTXT_SET_PARAMS` | `PTX_J83B_SYSTEM` と中心周波数 (90,000〜775,000 kHz)。`num_prop` は 0、`reserved` は 0 |
| `PTXT_TUNE` | `PTXT_SET_PARAMS` の値で選局する。ロックしない場合は `EAGAIN`、J.83B の firmware か helper がない場合は `ENOENT` |
| `PTX_START_STREAMING`、`PTX_STOP_STREAMING` | 配信の開始と停止。Bulk IN の受信は開いている間続ける |
| `PTX_GET_CNR` | 地デジの SNR、J.83B の MER を `DbToCndat()` で換算した値 |
| `PTX_GET_LOCK_STATUS` | 地デジは統計の `demod_locked`、J.83B は helper の TS が 3 秒以内に届いているか |
| `PTX_ENABLE_LNB_POWER` | 0 は成功、0 以外は `ENOSYS` |

受信機は 1 つのため、開けるのは同時に 1 つの利用者だけです。  
制御のタイムアウト、要求と応答の対応の喪失、形式の異常の後は、WinUSB 版と同じく自動では復旧せず、デバイスファイルを閉じるまで `EIO` を返します。  
helper だけが異常終了した場合は、WinUSB 版と同じく `read` を失敗させず、次の `PTXT_TUNE` で helper を起動し直せます。

## 機器の着脱とデーモンの寿命

USB の抜去時の扱いはほかの機種と同じで、開いている利用者がいる間はデバイスファイルを残し、受信は `EIO` を返します。  
抜去を検出したら helper を止め、未完了の転送の回収を待ってから `Device` を破棄します。  
抜去の後は firmware の読み込み直しを行いません。

デーモンの終了の手順 (`OnShutdown`) では、IQ の送出と helper を止め、J.83B 用の firmware なら通常の firmware へ戻してから受信を止めます。  
`smsusb` を外して機器を開いた場合は、機器を閉じるときに `libusb_attach_kernel_driver` で戻します (「決めた事項」の 5)。  
通常の firmware へ戻してから返すため、`smsusb` は地デジで稼働中の機器として扱える想定で、この想定は x86_64 の試験機で確かめます。

## 試験

### 自動試験

- `make -C cuse check` に、WinUSB 版の `siano_frontend_test` (コールド起動と読み込み直し、選局、統計、メッセージの形式、受信世代、IQ の配信の切り替え、J.83B の要求) を加える
- 共有のクラスへ切り出した場合は、firmware の切り替えの順序とメッセージの振り分けを模擬の USB で確かめる試験を加え、Windows と Linux の両方で実行する
- `PTXT_SET_PARAMS` の検査 (方式、周波数、`num_prop`、`reserved`) と、閉じた後の方式の戻りを `channel_test` などで確かめる
- helper の起動と停止 (停止の期限を過ぎた場合の `SIGKILL`、helper の異常終了、形式の異常) を、模擬の helper (シェルスクリプト) で確かめる
- `cargo build --release` と `cargo test` を Linux で実行し、Windows のビルドが変わらないことを `winusb/build.ps1` で確かめる

### 実機試験 (x86_64 の試験機、続いて検証環境)

1. 接続、`smsusb` との関係 (「決めた事項」の 5)、コールド起動と地デジで稼働中の機器を開く処理
2. 地デジの複数のチャンネルを 30 秒ずつ受信し、パケット数、同期・TEI・CC の異常、統計の誤りを記録する
3. 同じデバイスファイルを開いたままの地デジの切り替え (前のチャンネルの TS が混ざらないこと、TSID の一致)
4. IQ を記録して helper の処理量を測る (実時間に対する倍率)
5. J.83B (138.750 MHz など) を 60 秒受信し、WinUSB 版の表と同じ項目 (連続性エラー、MER、チップが落としたブロック、復調の作り直し、キューで捨てたブロック、RS の訂正不能) を記録する
6. 地デジと J.83B の交互の選局 (firmware の切り替え) と、J.83B の受信中のデバイスファイルの終了、デーモンの停止の後に `smsusb` と次の open が地デジとして動くこと
7. 受信中と待機中の USB の抜き差し、helper の強制終了
8. 地デジと J.83B の長時間受信

### Windows 版の回帰試験

「決めた事項」の 2 で共有のクラスへ切り出す場合は、切り出しの前後で次を比べます。

- `winusb/build.ps1` の x86・x64 のビルドと自動試験 (`siano_frontend_test` と、切り出しで加える試験)
- `siano_live_test` の地デジ (13ch、30 秒) と J.83B (138.750 MHz、60 秒) の受信を x64 と x86 で
- `siano_switch_test` の 13ch・14ch の交互選局 (x64 は 40 回)

共有のクラスへ切り出さない場合は、WinUSB 版のコードを変えないため、`winusb/build.ps1` の確認だけです。  
helper の `Cargo.toml` と `#[cfg(windows)]` の変更では、どちらの場合も Windows の helper のビルドと `cargo test` を確かめ、J.83B の受信は上の `siano_live_test` で確かめます。

## 実装

### 共有コードの切り出し

「決めた事項」の 2 のとおり、WinUSB 版の `Receiver` から OS に依存しない処理を [`siano/session.{hpp,cpp}`](../winusb/src/DriverHost_PX4/siano/session.hpp) の `siano::Session` へ移しました。  
`Session` は、開く処理と firmware の確認、地デジと J.83B の firmware の切り替えの順序、選局、統計と受信品質のログ、要求と応答の対応 (`Exchange`)、Bulk IN のメッセージの振り分け (前のホストの残りの読み飛ばし、TS、IQ のブロック、応答)、J.83B の helper との受け渡しを持ちます。  
OS ごとの処理は 2 つのインターフェースで受け取ります。

| インターフェース | 内容 | WinUSB 版 | Linux 版 |
|---|---|---|---|
| `SessionPort` | Bulk OUT の送信、抜去を見る待機、Bulk IN の受信の開始と停止、配信の世代と失敗の通知、firmware のファイルの読み込み | `siano/device.cpp` の `Receiver` (`WinUsb_WritePipe`、32 本の `WinUsb_ReadPipe`、INI と EXE の配置先からの firmware の探索) | `cuse/src/pxs1ud_device.cpp` の `PxS1udDevice` (libusb の同期 Bulk OUT と 32 本の非同期 Bulk IN、`/lib/firmware`) |
| `IqProcess` | J.83B の helper のプロセスの起動、IQ のブロックのキュー、停止 | `siano/iq_helper.cpp` の `IqHelper` (job object とパイプ) | `cuse/src/iq_process.cpp` の `PosixIqProcess` (`posix_spawn`、プロセスグループ) |

helper の標準出力の解釈 (`rtlsdr::Frames` による準備完了、TS、MER、エラーのフレーム) も [`siano/iq_helper_output.hpp`](../winusb/src/DriverHost_PX4/siano/iq_helper_output.hpp) の `HelperOutput` として共有しました。  
helper の TS の到着時刻は、Windows の `GetTickCount64()` から共有の `MonotonicMs()` (`steady_clock`) へ変えています。

切り出しで変えた動作は次の 2 点で、どちらも失敗の扱いの範囲です。

- 失敗の原因 (`error_`) は最初のものを残す。WinUSB 版は後の失敗で上書きしていたが、どの経路でも失敗の後は新しい要求を出さないため、記録される原因が変わるだけ
- Bulk OUT の送信の失敗を `SessionPort::Send` がログへ残す。Win32 のエラー番号は、送信と応答の待機をまとめて記録していた従来の行 (`command=... failed`) から、送信の行へ移った

WinUSB 版の `TsDiagnosticLog` と `TsTransferTracer` による転送単位の診断は、`Session::Receive` の TS の書き込みの関数 (呼び出し側が渡す) と戻り値のメッセージの情報から、従来どおり WinUSB 版の受信ループで行います。

`siano_session_test` (WinUSB 版の `winusb/tests/` に置き、`build.ps1` と `make -C cuse check` の両方で実行する) は、模擬の USB で次を確かめます。

- 地デジで稼働中の機器を開いたときに初期化を送り直さないこと、最初のメッセージの前の転送の読み飛ばし
- ロックの前の TS を配信しないこと、統計の SNR とロック
- J.83B の選局で、helper の起動が firmware の切り替えより先であること、読み込み直しの間だけ受信の失敗の回復を許すこと
- J.83B の後の地デジの選局と、閉じる処理で通常の firmware へ戻すこと
- helper の起動の失敗では open を失敗状態にしないこと、制御の失敗の後の操作が失敗を返すこと、同期の後の形式の異常

### Linux 版

`PxS1udDevice` は、受信のデバイスファイルを開いたときに `Session::Open` で Bulk IN の受信を始めて firmware を確かめ、閉じたときに `Session::Close` の後で受信を止めます。  
Bulk IN の完了の通知は libusb のイベントのスレッドで動き、`Session::Receive` で振り分けた TS を `VideoNode::Push` へ渡して、同じ転送を出し直します。  
J.83B 用の firmware の読み込み直しの間に受信が失敗した場合は、完了の通知の中では出し直さずに転送を保留し、制御のスレッドが次の送信と待機の前に halt を解除して出し直します (libusb の完了の通知から同期の転送を呼ばないため)。

選局は、地デジが `PTX_SET_CHANNEL` (方式の指定がなければ地デジ)、J.83B が `PTXT_SET_PARAMS` と `PTXT_TUNE` で、`video_node` に機種に依存しない拡張 ioctl の処理と、`VideoBackend` の `AcceptsParams` と `TuneParams` (既定は対応なし) を加えました。  
ほかの機種は `AcceptsParams` が false のため、`PTXT_SET_PARAMS` へは従来どおり `EINVAL` (以前の `ENOSYS` から、カーネルと同じ値) を返します。  
`main.cpp` の機種の表では、PX-BCUD と同じくカーネルのドライバーを外す印を付けました。

helper と J.83B 用の firmware は、`make -C cuse j83b` で作り、`make -C cuse install-j83b` で `$(PREFIX)/lib/px4-cused/` へ置きます。  
試験では、環境変数 `PX4_CUSED_LIBDIR` で導入前の場所を指定できます。

`msi2500_frontend` は、`windows-sys` を Windows だけの依存にし、MSi2500 の USB を使うサブコマンド (`capture`、`live`、`serve`) とその関数を `#[cfg(windows)]` で分けて、Linux では `serve-s1ud` と `demod` だけをビルドします。  
復調と誤り訂正のコードは変えていません。

### ビルドと自動試験

- Linux 版: x86_64 の試験機 (Debian 13 の GCC 14) で `make -C cuse` に新たな警告はなく、`make -C cuse check` は `siano_frontend_test`、`siano_session_test`、`iq_process_test` を含めてすべて成功しました
- helper と firmware: x86_64 の試験機の Debian の cargo 1.85 と binutils-arc-linux-gnu 2.44 で `make -C cuse j83b` が成功し、`s1ud_j83b.inp` の SHA-256 (`f4bb6b49…`) は WinUSB 版の配布物で固定している値と一致しました
- WinUSB 版: `siano_session_test` と `siano_frontend_test` は x86・x64 で成功しました

### Windows 版の回帰試験の結果

Windows 11 x64 の PC へ PX-S1UD を直接つなぎ、切り出しの前 (2026-11-20 にビルドした `siano_live_test` と `siano_switch_test`) と後で同じ試験を行いました。  
J.83B の比較には、この PC の試験用のフォルダーにあった J.83B 用の firmware (配布物とは別のファイル) を前後で同じものとして使い、配布物の firmware (`f4bb6b49…`) での受信は別に記録しました。

| 試験 | 切り出しの前 | 切り出しの後 (x64) | 切り出しの後 (x86) |
|---|---|---|---|
| 地デジ 13ch 30 秒 | 350,240 パケット、同期・TEI・CC・形式の異常 0、CNR 28〜30 dB | 349,760 パケット、異常 0 | 349,760 パケット、異常 0 |
| J.83B 138.750 MHz 60 秒 | 1,635,845 パケット、CC 5、MER 32.1 dB、チップが落としたブロック 0 | 1,635,687 パケット、CC 0、MER 31.7 dB、チップが落としたブロック 0 | 1,633,948 パケット、CC 40、チップが落としたブロック 11 |
| 13ch・14ch の交互選局 40 回 (3 秒ずつ) | 1,364,160 パケット、全区間の PAT の CRC と TSID が一致、異常 0。選局 1,762〜1,951 ms | 1,369,080 パケット、同じく一致、異常 0。選局 1,749〜2,102 ms | なし |

J.83B の x86 の CC の飛び 40 は、チップが落とした 11 ブロックの区間で、ホストの受信要求は 32 本中 31 本以上が残っていました ([px-s1ud.md](../winusb/docs/px-s1ud.md) の検証の表の x86 60 秒と同じ傾向)。  
地デジの最初の約 10 秒の受信品質のログに、ロックの前の統計 (`demod_locked=0`) が出ることは、切り出しの前のビルドでも同じでした。

配布物の J.83B 用の firmware (`f4bb6b49…`) での切り出しの後の x64 の 60 秒は、1,636,536 パケット、同期・TEI・CC・形式の異常 0、MER 31.6 dB、チップが落としたブロック 0 でした。  
同じ試験を `build.ps1` の実行中 (全論理プロセッサでコンパイルが動く間) に行った 1 回目は、受信要求の出し直しの間隔の最大が 7.2 ms (通常は 3〜4 ms) に延び、チップが 12 ブロックを落として、配信のバッファーで 2,124,024 バイトを拒否しました。  
CPU の負荷が J.83B の受信を崩すことは WinUSB 版の既知の傾向 (「Linux で測り直す値」の優先度の行) と同じで、切り出しによるものではありません。

`build.ps1` は、x86・x64 のビルド、`siano_session_test` を含む自動試験、`cargo test`、配布物の検査が成功しました。  
標準エラー出力をファイルへ向けて実行したため、最後の INI の検査の段階で PowerShell の `$ErrorActionPreference = 'Stop'` により終了コード 1 で止まりましたが、残りの 2 つの検査 (`ini_sections_test.py`、`check_ini_sections.py`) は個別に実行して成功しました。

## Linux 版の実機確認

x86_64 の試験機の USB ポートへ PX-S1UD を直接つなぎ、地上波と J.83B (スカパー！プレミアムサービス光) の信号を入力して確かめました。  
デーモンは、`px4-cused.service` と同じ制限 (`CapabilityBoundingSet=` 空、`NoNewPrivileges=yes`、`DevicePolicy=closed` と `/dev/cuse`・USB 機器だけの許可、`ProtectSystem=strict`) を `systemd-run` で付けて動かしました。  
導入前の helper と firmware は `PX4_CUSED_LIBDIR` で指定し、置き場所がホームディレクトリーの下のため `ProtectHome` だけは付けていません。  
J.83B の選局と受信には、`PTXT_SET_PARAMS`、`PTXT_TUNE`、`PTX_START_STREAMING` を呼んで TS を書き出す試験用の小さなプログラムを使いました (recpt1 は拡張 ioctl を使わないため)。

| 試験 | 結果 |
|---|---|
| 接続 | `smsusb` が機器を取って `/dev/dvb/adapter0` を作った後、デーモンが `smsusb` を外して `/dev/pxs1udvideo0` を作った。capability なしで外せた。Bulk IN `0x81`、応答の境界 504 バイト (WinUSB 版と同じ) |
| recpt1 で地デジ 13ch を 30 秒 | 337,700 パケット、同期・TEI・CC の異常 0、C/N 表示 30.0 dB、読み出しの間隔の最大 4 ms。ロックまで 2.0 秒 |
| 1 回開いたまま 13ch と 14ch を 3 秒ずつ 10 周期 (20 回の選局) | 676,440 パケット、同期・TEI・CC の異常 0。各区間の PAT の TSID は 31761 と 31762 で、WinUSB 版と同じ。別の TSID の PAT は 0。選局 1,655〜1,901 ms |
| J.83B 138.750 MHz の選局 (毎回、地デジの firmware から J.83B 用へ切り替え) | 6 回のうち 5 回が 0.93〜1.95 秒でロック (MER 31.1〜31.5 dB)。最初の 1 回は helper が作り直しを 21 回繰り返し (タイミングのずれ +17 ppm、MER 20.4 dB)、2 秒の上限で `EAGAIN` になった。WinUSB 版の「選局のロック待ち」の節で記録した、取り込みが誤った時間軸で始まる失敗と同じ形 |
| J.83B を 60 秒 | 1,567,299 パケット、利用者側の同期・TEI・CC の異常 0。helper は 99,593 ブロックを処理 (信号の時間 61.1 秒、キューで捨てたブロック 0)、チップが落としたブロック 0、MER 31.5 dB |
| J.83B の受信を閉じた後 | 閉じるたびに地デジの firmware へ戻した (`ISDB-T firmware reloaded`)。次の open で前の受信の残りを 1 回の転送だけ読み飛ばした (`skipped a stale USB transfer`) |
| J.83B の受信の後にデーモンを停止 | `smsusb` が戻り、地デジの firmware で稼働中の機器を初期化して (`Device initialized with return code 0`) `/dev/dvb/adapter0` を作り直した |
| 1 回開いたまま、地デジ 13ch (`PTX_SET_CHANNEL`) と J.83B 138.750 MHz (`PTXT_SET_PARAMS`、`PTXT_TUNE`) を 10 秒ずつ 10 周期 (20 回の選局と 20 回の firmware の切り替え) | 20 区間とも同期・TEI・CC の異常 0、`read` の失敗 0。各区間の PAT の TSID は地デジが 31761、J.83B が 12308 で、別の TSID の PAT は 0。地デジの選局は 1,998〜2,952 ms (J.83B の後は通常の firmware の読み込み直しを含む)、J.83B の選局は 930〜1,970 ms。helper のキューで捨てたブロック 0 |
| recpt1 で地デジ 13ch を 10 分 | 6,880,360 パケット (平均 17.25 Mbps)、同期・TEI・CC・形式の異常 0、読み出しの間隔の最大 5 ms |
| recpt1 で地デジ 13ch を 2 時間 (7,200 秒) | 82,657,860 パケット (平均 17.24 Mbps)、同期・TEI・形式の異常 0、CC の飛び 1、読み出しの間隔の最大 9 ms。デーモンの警告と異常のログは 0、受信の終わりのデーモンの RSS は 17,688 KB |
| J.83B の受信中にデーモンを停止 | 停止の要求から 0.9 秒で、IQ の送出の停止 (チップが落としたブロック 0)、helper の停止 (キューで捨てたブロック 0)、通常の firmware への読み込み直し、`smsusb` への返却の順に終えた。`smsusb` は機器を初期化し直して `/dev/dvb/adapter0` を作った。停止までの 514,953 パケットは異常 0 で、その後の `read` は `ECONNABORTED` (CUSE のセッションの終了) |
| 待機中の USB の抜き差し | 抜去を検出してデバイスファイルを消し、挿し直した機器から再び `smsusb` を外して `/dev/pxs1udvideo0` を作った |
| recpt1 で地デジ 13ch を受信中に USB を抜き、約 10 秒後に同じポートへ挿し直す | 抜去までの 208,060 パケット以上は異常 0。挿し直した機器は `/dev/pxs1udvideo1` で提供し (古いデバイスファイルを recpt1 が開いていたため)、recpt1 を閉じた後に古いデバイスファイルは消えた。挿し直した機器で 13ch を 15 秒受信し、異常 0 |
| J.83B を受信中に USB を抜き、約 10 秒後に同じポートへ挿し直す (修正の後) | 抜去までの 606,113 パケットは異常 0。`read` は `EIO`、helper は止まり、IQ の停止の要求は送らなかった (`bytes=0`)。挿し直した機器で J.83B を 30 秒受信し、809,383 パケット、異常 0、MER 31.5 dB |
| 地デジ 13ch を 1 回の選局のまま受信中に USB を抜き、約 10 秒後に同じポートへ挿し直す (`StopStream` が `EIO` を返す修正の後) | 抜去までの 161,580 パケットは異常 0、`read` は `EIO`。挿し直した機器は `/dev/pxs1udvideo0` で提供し、13ch を 15 秒受信して 171,580 パケット、異常 0。デーモンを止めると `smsusb` へ戻った |
| 地デジの 1 秒ごとの選局の繰り返しの途中で USB を抜く (同じ修正の後) | 選局中の `PTX_SET_CHANNEL` は `ECANCELED`、その後の選局は `EIO` を返した |

2 時間の受信の CC の飛びは、19:00:01 の PID 0x0111 の 1 件 (CC が 2 から 11) で、同じ時刻の同期と TEI の異常はありませんでした。  
PIX-DT295 の 6 時間の受信 ([20261120_dt295_linux_cuse_design.md](20261120_dt295_linux_cuse_design.md)) でも、同じ PID 0x0111 で番組の切り替わりの 1 秒後 (12:45:01 など) に CC の飛びが記録され、副音声の ES の送出が番組の切り替わりで止まって再開したものと判断しています。  
今回も PID と時刻の形が同じため、放送側の送出の再開によるものと見ています。  
ただし、計数の道具が PID ごとの途切れの時間を記録しないため、0x0111 がその直前に途切れていたことは確かめていません。

受信中の抜去の最初の試験では、`read` が `EIO` ではなく `ECANCELED` を返しました。  
`VideoNode::Remove` は受信を止めてから `EIO` を設定しますが、受信を止める `PxS1udDevice::StopStream` が、抜去の後に `Session::SetCapture` の返す `-ECANCELED` を受信中の異常として返し、先にリングの終わりの値になっていたためです。  
抜去の後の `StopStream` は異常を返さないように直し、上の表の J.83B の再試験で `EIO` になることを確かめました。地デジの受信中の抜去の 1 回目は修正の前の試験で、recpt1 は `read` の失敗の値を記録しないため、その時点の値は確かめていません。  
その後、RTL-SDR の実機で、抜去の後の `StopStream` が 0 を返すと、`VideoNode::Remove` が `EIO` を設定する前 (`OnRemove` の間) に待機中の `read` が EOF を受け取ることが分かりました ([20261122_rtlsdr_linux_cuse_design.md](20261122_rtlsdr_linux_cuse_design.md) の「Linux 版の実機確認」)。  
PX-S1UD の `StopStream` も抜去の後に `EIO` を返すように直し、上の表の地デジの再試験で `read` が `EIO` になることを確かめました。修正の前の EOF は PX-S1UD の実機では再現していません。

地デジの受信中の抜き差しの後、挿し直した機器で 13ch の 15 秒の受信を終えた約 13 秒後 (受信の再開から約 12 秒)、機器が利用者の操作なしに USB から外れ、約 1 秒後に別の機器番号で付き直しました。  
直前の約 20 ms に Bulk IN の転送の失敗 (`LIBUSB_TRANSFER_ERROR`) が続き、デーモンはその open を形式の異常で失敗状態にしてから、付き直した機器を提供しました。  
付き直した機器での 120 秒の地デジの受信は異常 0 で、この現象は再現していません。  
J.83B の受信中の 1 回目の抜去では、挿し直した機器が USB に現れず、利用者が挿し直すと認識されました。  
どちらも 1 回だけの観測で、機器とポートの接触、チップのリセット、ホストのどれが原因かは分かっていません。

J.83B の firmware の切り替えで、WinUSB 版で起きた Bulk IN の受信の失敗 (ROM へ戻るときの `ERROR_GEN_FAILURE`) は、Linux では 1 度も起きませんでした。  
保留した転送を出し直す経路は、実機では通っていません。

### 検証環境 (Raspberry Pi 4) での地デジ

検証環境の USB ポート (本体内の VL805 の内蔵ハブを経由) へ PX-S1UD をつなぎ、同じ制限を付けた試験用のデーモンで地デジを確かめました。  
運用中の `px4-cused.service` は試験の間だけ止め、`isdbt_rio.inp` は `firmware-siano` パッケージで入れました。  
検証環境のカーネルも `smsusb` で機器を取っていたため、デーモンは x86_64 の試験機と同じく `smsusb` を外して `/dev/pxs1udvideo0` を作りました。

| 試験 | 結果 |
|---|---|
| recpt1 で 13ch を 30 秒 | 343,460 パケット、同期・TEI・CC の異常 0、C/N 表示 30 dB。ロックまで 1.9 秒。開く処理で、`smsusb` が残した転送を 1 回だけ読み飛ばした |
| 1 回開いたまま 13ch と 14ch を 3 秒ずつ 10 周期 (20 回の選局) | 676,420 パケット、同期・TEI・CC の異常 0。PAT の TSID は 31761 と 31762 で、別の TSID の PAT は 0。選局 1,659〜1,901 ms |
| recpt1 で 13ch を 10 分 | 6,877,420 パケット (平均 17.12 Mbps)、同期・TEI・CC・形式の異常 0、読み出しの間隔の最大 5 ms |
| デーモンの資源 (10 分の受信の終わり) | CPU は 1 コアの約 4%、RSS 17,264 KB |

地デジの受信には、VL805 の内蔵ハブを経由した Bulk 転送で欠落はありませんでした。  
J.83B は「J.83B の helper の処理量」のとおり検証環境の CPU では実時間に足りない見込みのため、試していません。

### J.83B の helper の処理量

60 秒を超える J.83B の受信では、x86_64 の試験機の helper が実時間に間に合いませんでした。  
180 秒の受信では開始から約 60 秒の後、120 秒の受信では途中から、デーモンと helper の間のキューがあふれ (180 秒で 53,920 ブロック、120 秒で 25,248 ブロック)、復調の作り直しと CC の飛び (180 秒で 6,314) が続きました。  
チップが落としたブロックは 0 で、USB の受信は欠けていません。

| 項目 | 値 |
|---|---|
| helper のスレッドごとの CPU (受信の 30 秒後) | 前処理と復調 (`s1ud-qam`) 99.9%、誤り訂正 (`j83b-fec`) 76%、そのほか 5% 未満 |
| helper 全体の CPU | 1 コアの 150〜163% |
| デーモンの CPU | 1 コアの約 4% |
| CPU のクロック (4 コアの平均) | 2,600〜2,900 MHz (下がり続ける様子はなかった) |
| 試験機の負荷 | 4 台の仮想マシンが動き、load average が 1.9 から 4.1 へ上がった |

前処理と復調のスレッドが 1 コアを使い切っており、Intel N150 では、ほかの負荷がある状態で実時間を保てません。  
Ryzen 9 5900X で実時間の約 0.64 倍だった処理が、N150 ではほぼ 1 倍に当たります。  
最初の 60 秒の受信が欠けなかったのは、試験機のほかの負荷が低かった間だったためと考えていますが、負荷のない状態では確かめていません。  
検証環境 (Cortex-A72、SIMD の処理なし) は、この結果から J.83B の実時間の処理に足りない見込みで、受信を試していません。

この結果から、Linux 版の J.83B は、前処理と復調の 1 スレッドが N150 より十分に速い x86_64 の CPU を前提とします。  
helper の高速化 (前処理と復調のスレッドの分割など) と、デーモンから helper の優先度を上げることは、この対応の範囲外として見送りました。

## 決めた事項

利用者の判断を仰いだ事項と、その結果は次のとおりです。

| 事項 | 選択肢 | 設計者の推奨 | 決定 |
|---|---|---|---|
| 1. 受信のデバイスファイルの名前 | `pxs1udvideoN` / `s1udvideoN` | `pxs1udvideoN` (`pxbcudvideoN` と同じく製品名の型番から作る) | `pxs1udvideoN` (2026-11-22) |
| 2. WinUSB 版との共有の範囲と順序 | A. OS に依存しないファイルだけを共有し、受信と firmware の切り替えは Linux 版で書く / B. firmware の切り替えの順序とメッセージの振り分けを共有のクラスへ切り出し、Windows の回帰試験を済ませてから Linux 版を作る | B (実機で見つけた切り替えの条件を OS ごとに複製しない) | B (2026-11-22) |
| 3. J.83B の復調の実装場所 | A. `msi2500_frontend` を Linux でもビルドし、`px4-cused` が子プロセスとして起動する / B. Linux 版は地デジだけにする | A (GPL-3.0 の helper は GPL-2.0-only のデーモンへ組み込めず、WinUSB 版と同じ構成が使える) | A (2026-11-22) |
| 4. firmware の入手と配置 | 地デジ: linux-firmware の `/lib/firmware/isdbt_rio.inp` を読む / J.83B: A. `make -C cuse` の別の目標で ARC の binutils から作って `/usr/local/lib/px4-cused/` へ導入する / B. ビルド済みの `s1ud_j83b.inp` をリポジトリへ登録する / C. 実行ファイルへ埋め込む | 地デジは linux-firmware、J.83B は A (ソースから作れる GPL の firmware で、J.83B を使わない利用者のビルドに ARC の binutils を求めない) | 地デジは linux-firmware、J.83B は A (2026-11-22) |
| 5. カーネルの `smsusb` との関係 | A. `px4-cused` が機器を開くときに `smsusb` を外し、閉じるときに戻す (PX-BCUD と同じ) / B. `smsusb` を blacklist する設定を配布する | A (`smsusb` はほかの Siano の機器も扱い、blacklist はそれらを使えなくする) | A (2026-11-22) |
| 6. 最初の実機試験の環境 | x86_64 の試験機 / 検証環境 | x86_64 の試験機 (`smsusb` との関係を確かめられ、helper の処理量の余裕が大きい) | x86_64 の試験機 (2026-11-22)。J.83B の信号も入力できる |

設計者が決めた次の事項は、異論があれば見直します。

- J.83B の選局はカーネルと同じ拡張 ioctl (`PTXT_SET_PARAMS`、`PTXT_TUNE`) で行い、`video_node` へ機種に依存しない処理として加える
- C/N は地デジの SNR と J.83B の MER を `DbToCndat()` で換算する
- J.83B のロックの待機は WinUSB 版と同じ 2 秒
- helper の標準エラー出力は journal へ送る
- ISDB-C、アンテナ給電、カードのデバイスファイルは対象外

## 未確認の条件

「Linux 版の実機確認」で、`smsusb` との取り合い、地デジの受信と切り替え、J.83B の短時間の受信、1 回開いたままの地デジと J.83B の交互の選局、firmware の切り替えと戻し、待機中と受信中の抜き差しは確かめました。  
次の条件は未確認です。

- 挿し直しの後に 1 度起きた、操作なしの USB の切断と再接続の原因
- J.83B の 60 秒を超える受信を、実時間に間に合う CPU で行うこと
- 負荷のない N150 での helper の処理量と、検証環境 (Cortex-A72) の処理量
- firmware の読み込み直しの間の Bulk IN の失敗と、保留した転送を出し直す経路 (Linux では起きなかった)
- 検証環境での J.83B の受信 (CPU の処理量から対象外の見込み)
- `smsusb` が機器を手放すときのカーネルの警告による、メモリーの影響の大きさ
- 地デジの 2 時間を超える受信と、13ch・14ch 以外のチャンネル
