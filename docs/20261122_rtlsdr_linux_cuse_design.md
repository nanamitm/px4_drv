# RTL-SDR のワンセグの Linux 対応の設計 (px4-cused)

この文書は、RTL-SDR (USB 側の名称 RTL2838UHIDIR、USB ID `0bda:2838`、チューナー R820T) で地デジのワンセグを Linux で受信するための構成案です。  
PIX-DT295 ([20261120_dt295_linux_cuse_design.md](20261120_dt295_linux_cuse_design.md))、Sony torne CECH-ZD1J ([20261121_torne_linux_cuse_design.md](20261121_torne_linux_cuse_design.md))、I-O DATA GV-MVP/XZ3 ([20261121_gv_mvp_xz3_linux_cuse_design.md](20261121_gv_mvp_xz3_linux_cuse_design.md))、PLEX PX-BCUD ([20261122_px_bcud_linux_cuse_design.md](20261122_px_bcud_linux_cuse_design.md))、PLEX PX-S1UD ([20261122_px_s1ud_linux_cuse_design.md](20261122_px_s1ud_linux_cuse_design.md)) と同じく、ユーザー空間のデーモン `px4-cused` が libusb で機器を動かし、CUSE でデバイスファイルを公開する方式を前提にします。  
維持する契約、WinUSB 版と共有するコード、Linux で測り直す値、試験の範囲、利用者の判断を仰ぐ事項を整理します。  
x86_64 の試験機での確認の結果は「Linux 版の実機確認」に、確かめていない条件は「未確認の条件」にまとめています。  
WinUSB 版での測定値は、出典の文書 ([rtlsdr-bondriver.md](../winusb/docs/rtlsdr-bondriver.md)) から引用します。

この文書の「x86_64 の試験機」は、USB ハブを経由せずに機器をつなげる x86_64 の Proxmox VE のホスト (Intel N150、4 コア、Debian 13、カーネル 6.17) を指します。  
「検証環境」は、ほかの機種の設計文書と同じ Raspberry Pi 4 Model B (64 ビット版の Raspberry Pi OS Lite) を指します。

## 目的と範囲

Linux で RTL-SDR を使い、UHF 13〜52ch の中央の 1 セグメント (ワンセグ) を受信できるようにすることが目的です。  
recpt1、BonDriverProxy_Linux、Mirakurun などは、既存の機種と同じく受信のデバイスファイルを開き、PTX の ioctl (`PTX_SET_CHANNEL`) で選局できます。  
得られる TS はワンセグのサービスで、SDT に記載されたフルセグを受信する機能ではありません。

RTL2838UHIDIR は復調器を持たず、R820T で選んだ周波数の IQ (約 1.016 MS/s、8 ビットの符号なし) を USB へ送ります。  
OFDM の復調から誤り訂正、TS の組み立てまでをホストが行い、WinUSB 版ではこの処理を helper (`rtlsdr_frontend`、`vendor/isdbt-dsp`) が担います。

内蔵カードリーダーとアンテナ給電はなく、カードのデバイスファイル (`/dev/px4card/<ID>`) は作らず、`PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS` を返します。  
BS・CS、VHF と CATV のパススルーの周波数、R820T 以外のチューナーは対象外です。

## 汎用の SDR であることの影響

RTL2838UHIDIR は、ワンセグ以外の用途 (AIS、ADS-B、無線の受信など) に広く使われる汎用の SDR です。  
x86_64 の試験機にも、仮想マシン (AIS の受信) が使っている RTL2838 が 2 本つながっています。

| sysfs のポート | シリアル番号 | 使用者 |
|---|---|---|
| `3-1` | `00000060` | 仮想マシンの QEMU (`usbfs`) |
| `3-3` | `00000001` | 仮想マシンの QEMU (`usbfs`) |
| `3-4` | `00000001` | 試験に使う機器。カーネルの `dvb_usb_rtl28xxu` が interface 0 を取っている |

既存の機種と同じく VID と PID だけで機器を開くと、`px4-cused` はほかの用途の RTL2838 も開いてしまいます。  
また `3-3` と `3-4` はシリアル番号が同じで、RTL-SDR の多くの製品は工場出荷時のシリアル番号 `00000001` を持つため、シリアル番号では個体を区別できません。  
シリアル番号を読むには機器を開いて文字列の記述子を要求する必要があり、ほかのプロセスが使っている機器へ制御転送を送ることにもなります。

このため RTL-SDR は、利用者が明示した USB の接続位置の機器だけを、開く前に選ぶ方式が必要です (「決めた事項」の 4)。  
指定がない場合、`px4-cused` は RTL2838 を 1 本も開きません。

## カーネルの dvb_usb_rtl28xxu との関係

Linux の本体の `dvb_usb_rtl28xxu` は `0bda:2838` を対応機種として持ち、x86_64 の試験機でも `3-4` の interface 0 を取って、`rtl2832`、`r820t`、`rtl2832_sdr` を読み込んでいます。  
このドライバーは DVB-T の復調と、V4L2 の SDR のデバイス (`/dev/swradioN`) を提供しますが、ISDB-T の復調は持ちません。  
`px4-cused` と同じ interface を取り合うため、PX-BCUD の `em28xx`、PX-S1UD の `smsusb` と同じ扱いが必要です (「決めた事項」の 5)。

interface 1 (赤外線) は、x86_64 の試験機ではどのドライバーにも結び付いていません。  
受信には interface 0 だけを使い、interface 1 には触れません (WinUSB 版も interface 0 だけを WinUSB へ結び付けています)。

## ほかの機種との違い

| 項目 | PX-S1UD | RTL-SDR |
|---|---|---|
| USB ブリッジ | Siano SMS2270 | Realtek RTL2832U (復調器を使わず、ADC の IQ を送る) |
| 制御 | Bulk OUT の Siano のメッセージ | vendor request の制御転送で RTL2832U のレジスターを書き、I2C の中継で R820T を設定する |
| 受信できる放送 | 地デジ、J.83B (実験的) | 地デジのワンセグ (UHF 13〜52ch) |
| TS の取り出し | Bulk IN の Siano のメッセージ | Bulk IN `0x81` の IQ を helper で復調した TS |
| USB の流量 | 地デジの TS で約 2.3 MB/s | IQ で約 2.03 MB/s (1,015,873 サンプル/秒 × 2 バイト) |
| firmware | 必要 | 不要 |
| ホストの処理量 | 地デジは TS の切り出しだけ | すべての受信で OFDM の復調と誤り訂正 |
| 選局からの最初の TS | 約 2 秒 | WinUSB 版の x64 で 4.21〜5.11 秒 |
| 個体の区別 | 同じ製品を 1 台だけ想定 | シリアル番号では区別できず、ほかの用途と取り合う |

## 構成

`px4-cused` に 6 つ目の機種として `RtlSdrDevice` を追加します。  
`Device` 基底クラス、ホットプラグ、受信のリングと PTX の ioctl (`video_node`) は、ほかの機種と同じものを使います。

WinUSB 版では helper が librtlsdr (`rtlsdr.dll`、RTL-SDR Blog V1.4.0) で USB を開き、IQ の取得と復調の両方を行い、DriverHost_PX4 は helper の寿命と TS の配信だけを受け持ちます。  
Linux 版で USB をどちらが持つかには、次の 2 案があります (「決めた事項」の 2)。

| 項目 | A. デーモンが USB を持ち、helper は復調だけ | B. WinUSB 版と同じく helper が USB を持つ |
|---|---|---|
| 機器の選び方 | デーモンが接続位置で選んでから開く (ほかの機器を開かない) | librtlsdr は番号かシリアル番号で選ぶ。番号を求めるには全機器の文字列を読み、`3-3` と `3-4` は同じシリアル番号のため区別できない |
| カーネルのドライバー | ほかの機種と同じく、開くときに外し閉じるときに戻す | librtlsdr のビルドの設定 (`DETACH_KERNEL_DRIVER`) に依存する |
| RTL2832U と R820T の制御 | デーモンが行う。RTL-SDR Blog V1.4.0 の librtlsdr のソース (WinUSB 版の `rtlsdr.dll` と同じ版) を使う | helper が Debian の `librtlsdr0` (osmocom 2.0.2) を使う。WinUSB 版と版が異なる |
| IQ の取得 | デーモンの libusb の非同期 Bulk 転送 (15 本 × 256 KiB、librtlsdr の既定と同じ) | helper の librtlsdr の `rtlsdr_read_async` |
| helper | 標準入力で IQ を受け取る形を加える。USB を開く権限が不要 | WinUSB 版の `rtlsdr.dll` の読み込みを `librtlsdr.so.0` へ置き換える |
| 抜去の検出 | デーモンのホットプラグ (ほかの機種と同じ) | helper の USB の失敗の後に、デーモンのホットプラグで確かめる |
| systemd の制限 | 変更不要。helper は USB を開かない | helper も USB 機器を開くため、`px4-cused.service` の制限を helper も受ける |

推奨は A です。  
B の機器の選び方は、x86_64 の試験機の構成で仮想マシンの機器を開く危険を避けられません。  
A では、WinUSB 版の helper と復調の処理 (`isdbt-dsp`) はそのまま使い、USB の処理だけが WinUSB 版と分かれます。

| デバイスファイル・プロセス | 内容 |
|---|---|
| `/dev/rtlsdrvideoN` (「決めた事項」の 1) | 受信。PTX の ioctl、`read`、`poll` |
| helper (`rtlsdr_frontend --stdin`) | 選局のたびに `px4-cused` が子プロセスとして起動し、標準入力で IQ を受けて、標準出力へ TS と状態を返す (A の場合) |

### 機種ごとの処理の境界 (A の場合)

| 層 | 共有するもの | Linux 版だけのもの |
|---|---|---|
| helper との通信の形式 (`rtlsdr::Frames`) | WinUSB 版の `rtlsdr/protocol.hpp` をそのまま使う | なし |
| 周波数の検査と換算、helper の監視と自動の起動し直し、ロックと C/N の判定 | 「決めた事項」の 3 による | 「決めた事項」の 3 による |
| helper のプロセスの起動と停止 | なし | `posix_spawn`、パイプ、プロセスグループへの `SIGKILL` (PX-S1UD の `iq_process.cpp` と同じ処理) |
| RTL2832U と R820T の制御 | 上流の librtlsdr (RTL-SDR Blog V1.4.0) | 開いた libusb のハンドルを librtlsdr へ渡す入口 |
| Bulk IN の受信 | なし | libusb の非同期 Bulk 転送と、helper の標準入力へのキュー |
| C/N | なし | helper の MER (dB) を `DbToCndat()` で PT1/PT3 互換の値へ換算する |

## WinUSB 版と共有するコード

[`rtlsdr/protocol.hpp`](../winusb/src/DriverHost_PX4/rtlsdr/protocol.hpp) (helper の出力の形式と検査) は Windows の API を使っておらず、PX-S1UD の J.83B でもすでに Linux 版と共有しています。

[`rtlsdr/rtl_device.cpp`](../winusb/src/DriverHost_PX4/rtlsdr/rtl_device.cpp) の `Receiver` は、Windows の API (`CreateProcessW`、job object、`PeekNamedPipe`、`CreateFileW` によるデバイスインターフェースの確認) と、次の OS に依存しない処理を 1 つのクラスに持っています。

- 周波数の検査 (473,143〜707,143 kHz、6 MHz の格子) と、helper へ渡す Hz への換算 (473,142,857 Hz + 6 MHz × (チャンネル − 13))
- helper の出力の解釈 (準備完了、TS、MER と品質、エラー) と、3 秒以内の TS の到着によるロックの判定
- helper が予期せず終わったときの起動し直し (PX-BCUD と同じ `RecoveryBudget` で連続 8 回まで、2 回目から 250 ms 刻みで最大 2 秒の待機、5 秒を超えて受信できた後は連続回数を忘れる)、`helper restart` と `outage_ms` のログ、使い切った後の扱い
- 起動し直す前に抜去の通知を最長 3 秒待つ処理 (Windows は helper の USB の失敗が抜去の通知より先に届くため)

起動し直しの条件と回数は、WinUSB 版の実機の長時間受信で決めた値です。  
Linux 版で書き直すと、片方の実機で見つけた条件がもう片方へ反映されない危険があるため、PX-S1UD の `siano::Session` と同じく、プロセスと待機以外の処理を共有のクラス (`rtlsdr/session.{hpp,cpp}`) へ切り出してから Linux 版を作る案 (「決めた事項」の 3 の B) を推奨します。  
この案では WinUSB 版の `Receiver` の中身が変わるため、Windows の実機で切り出しの前後の受信を比べます (「Windows 版の回帰試験」)。

切り出す `rtlsdr::Session` は、OS ごとの処理を次のインターフェースで受け取る想定です。

| インターフェース | 内容 | WinUSB 版 | Linux 版 |
|---|---|---|---|
| `HelperPort` | helper の起動 (周波数と利得)、標準出力の読み取り (期限付き)、強制終了と回収、機器がまだあるか | `CreateProcessW`、job object、`PeekNamedPipe`、デバイスインターフェースのパス | `posix_spawn`、`poll`、プロセスグループ、`Device::Removed()` |

A の Linux 版では、helper の起動のたびに Bulk IN の受信も始め直し (RTL2832U の FIFO のリセットを含む)、USB の受信の失敗も helper の失敗と同じ回数の制限で数えます。  
WinUSB 版では helper が USB を持つため、USB の失敗は helper の終了として現れ、同じ経路で起動し直しています。

### ライセンス

`px4-cused` と DriverHost_PX4 は GPL-2.0-only です。  
librtlsdr (RTL-SDR Blog V1.4.0) は GPL-2.0-or-later のため、A ではデーモンへそのまま組み込めます。  
helper (`rtlsdr_frontend`) と `isdbt-dsp` は GPL-3.0-or-later で、GPL-2.0-only のデーモンへは組み込めず、WinUSB 版と同じく別のプロセスとして起動し、標準入出力だけで通信する構成が必要です。

## RTL-SDR 固有の扱い

### 機器の選び方

接続位置は sysfs と同じ「バス番号-ポートの経路」(例 `3-4`、ハブの先は `3-2.1`) で、環境変数 `PX4_CUSED_RTLSDR_PORTS` (カンマ区切り) に指定する案を推奨します (「決めた事項」の 4)。  
systemd では `systemctl edit px4-cused` のドロップインで `Environment=PX4_CUSED_RTLSDR_PORTS=3-4` を加えます。  
`px4-cused` は、ホットプラグの通知で `libusb_get_bus_number` と `libusb_get_port_numbers` (機器を開かずに sysfs から得られる) を比べ、一致した機器だけを `libusb_open` します。

開いた後に、librtlsdr が判定したチューナーが R820T (R820T2 を含む `RTLSDR_TUNER_R820T`) であることを確かめ、それ以外は初期化の失敗としてログへ残し、カーネルのドライバーを戻して手放します。  
WinUSB 版の helper はチューナーを確かめていませんが、実機で確かめたのは R820T だけで、ほかのチューナーの利得の一覧と IF の設定は確かめていないためです。  
シリアル番号は、ログの表示にだけ使います。

機器を別のポートへ挿し替えた場合は、指定を変えるまで使えません。  
ポートの経路はハブの構成でも変わるため、`README` で `lsusb -t` と sysfs (`/sys/bus/usb/devices/*/serial`) による調べ方を説明します。

### カーネルのドライバー

推奨は、PX-S1UD と同じく、指定した機器を開くときに `libusb_detach_kernel_driver` で interface 0 から `dvb_usb_rtl28xxu` を外し、閉じるときとデーモンの終了の手順で `libusb_attach_kernel_driver` で戻す方式です (「決めた事項」の 5 の A)。  
`main.cpp` の機種の表の `detach_kernel_driver` を使い、ほかの機種と同じ処理になります。  
`dvb_usb_rtl28xxu` の blacklist (B) は、指定していない RTL2838 の DVB-T と SDR の使い方も変え、ホストのモジュールの設定を恒久的に変えるため、推奨しません。

`dvb_usb_rtl28xxu` が残した RTL2832U と R820T の状態は、librtlsdr の開く処理 (`rtlsdr_init_baseband` とチューナーの初期化) で作り直す想定です。  
外した直後の Bulk IN に残る古い IQ は、受信の開始時の FIFO のリセット (`rtlsdr_reset_buffer` と同じ処理) と、helper の起動のたびの受信世代で配信しません。

### RTL2832U と R820T の制御 (A の場合)

librtlsdr の `rtlsdr_open` は自分で libusb を初期化し、番号で機器を探して開くため、開いたハンドルを渡す入口 (`rtlsdr_open_handle` などの名前) を加えます。  
そのほかの処理 (`rtlsdr_set_center_freq`、`rtlsdr_set_sample_rate`、`rtlsdr_set_tuner_gain_mode`、`rtlsdr_set_tuner_gain`、`rtlsdr_get_tuner_gains`、`rtlsdr_reset_buffer`) は WinUSB 版の helper と同じ順に呼びます。  
IQ の取得は librtlsdr の `rtlsdr_read_async` を使わず、デーモンの libusb のイベントのスレッドで非同期 Bulk 転送を回します (`rtlsdr_read_async` は自分のイベントのループを持ち、デーモンの通知の処理と重なるため)。

librtlsdr のソースの入手方法は「決めた事項」の 6 で決めます。  
WinUSB 版の `build.ps1` は、同じ版のソースの ZIP を SHA-256 (`EFAAC50D…`) で固定しています。

### 選局

`PTX_SET_CHANNEL` の `freq_no` を、ほかの機種と同じ `channel.hpp` の `ChannelToTune()` で中心周波数 (kHz) へ換算します。  
UHF の 13〜52ch (`freq_no` 63〜102) だけを受け付け、`slot` (周波数の補正値) が 0 でない要求、VHF、CATV のパススルー、衛星は `EINVAL` とします。  
helper の復調は中心周波数の 1/7 MHz のずれを前提とし、WinUSB 版も格子から外れた周波数を断っているためです。

選局では、前の helper と IQ のキューを止め、新しい復調器と PAT の状態で helper を起動します。  
前のチャンネルの IQ、TS の端数、continuity counter は、helper のプロセスごと捨てるため、次のチャンネルへ引き継がれません。

最初の TS までに WinUSB 版の x64 で 4.21〜5.11 秒かかり、ほかの機種のロックの待機 (3 秒) より長くなります。  
`PTX_SET_CHANNEL` の完了をどこで返すかには、次の 2 案があります (「決めた事項」の 7)。

| 案 | 内容 | 影響 |
|---|---|---|
| A. 最初の TS を待つ | 最長 15 秒、helper の最初の TS を待って成功を返す。届かなければ `EAGAIN` | カーネルの契約 (成功はロックを意味する) と同じ。信号のないチャンネルでは 15 秒かかる |
| B. 起動で返す | helper を起動した時点で成功を返し、ロックは `PTX_GET_LOCK_STATUS` で知らせる (WinUSB 版の `AsyncTune=true` と同じ) | 選局は速いが、信号のないチャンネルでも成功を返し、recpt1 は TS のない録画を続ける |

推奨は A です。  
録画の用途では、選局の失敗を `EAGAIN` で知らせ、再試行する利用者の処理を働かせる方が、TS のない録画を続けるより損害が小さいためです。  
15 秒は、WinUSB 版の `AsyncTune=false` の `TuneTimeout` (20 秒) より短く、N150 の helper の処理量を測ってから見直します。

### helper の監視と自動の起動し直し

WinUSB 版と同じく、helper が予期せず終わったとき (クラッシュ、形式の異常、30 秒の無応答、IQ の 5 秒の停止) と、A の Bulk IN の受信の失敗のときは、同じ周波数で helper と受信を起動し直します。  
回数の制限、待機、連続回数を忘れる条件は WinUSB 版と同じ `RecoveryBudget` で、起動し直す間も `read` は失敗させず、欠けた区間は利用者が continuity counter で検出します。  
回数を使い切った後は `PTX_GET_LOCK_STATUS` で未ロックを返し、次の選局を待ちます。

Linux 版では抜去をホットプラグで受け取るため、起動し直す前の待機は `Device::Removed()` の確認に置き換わります。  
抜去の後は起動し直しません。

IQ のキュー (デーモンから helper の標準入力まで) があふれた場合は、WinUSB 版と同じく IQ を捨てて復調を続ける扱いを避け、失敗として記録して起動し直します (「I/Q を捨てて復調を継続する扱いは、欠落を隠すため対象外」)。

### helper の Linux 向けのビルド

`rtlsdr_frontend` の依存は `isdbt-dsp` (このリポジトリの `vendor/` に固定したソース) だけで、Windows に依存するのは `rtlsdr.dll` を読み込む `kernel32` の部分です。  
A では、標準入力から IQ を読む `--stdin` を加え、`rtlsdr.dll` の部分を `#[cfg(windows)]` で分けます。  
`--stdin` は既存の `--replay` (保存した IQ の再生) と同じく `Input::Iq` を送り、`--replay` と異なり IQ の 5 秒の停止を失敗として扱い、標準入力の EOF を停止の要求として扱います。  
WinUSB 版の helper の動作は変えず、Windows のビルドと `cargo test` で確かめます。

helper の標準エラー出力は、PX-S1UD と同じくデーモンの標準エラー出力 (journal) へつなぎます (`px4-cused.service` は `ProtectSystem=strict` でログのファイルを書く場所を持たないため)。  
helper は `make -C cuse rtlsdr` で作り、`make -C cuse install-rtlsdr` で `$(PREFIX)/lib/px4-cused/rtlsdr_frontend` へ置きます (Rust を必要とするため、既定の `all` には含めない)。

### C/N

helper が TS の出力中に報告する復調器の MER (dB) を、torne の DiB8000 と同じ `DbToCndat()` で、recpt1 などの地デジの式で同じ dB になる値へ換算して `PTX_GET_CNR` で返します。  
MER を受け取る前と TS が 3 秒届いていない間は、換算の下限 (約 3.1 dB) を返します。  
WinUSB 版は MER の前に RS の成功割合を返しますが、成功割合は dB の量ではなく、PT1/PT3 互換の値へ換算できないためです。  
MER は RF の C/N とは一致せず、WinUSB 版の検証では既定の利得 (29.7 dB) で 12〜14 dB、利得を下げると上がりました ([rtlsdr-bondriver.md](../winusb/docs/rtlsdr-bondriver.md) の「MER による信号レベル」)。

### 利得

WinUSB 版の `GainTenthsDb` (既定 297、0.1 dB 単位) と同じ値を、環境変数 `PX4_CUSED_RTLSDR_GAIN` で変えられるようにします。  
x86_64 の試験機のアンテナの条件は Windows の検証環境と異なるため、既定の利得で MER を測り、入力が過大なら値の目安を文書へ残します。

### Linux で測り直す値

| 値 | WinUSB 版 | Linux 版の扱い |
|---|---|---|
| Bulk IN の転送 | librtlsdr の既定 (15 本 × 256 KiB、IQ の約 1.9 秒分) | 同じ値で始める |
| helper の処理量 | x86 の初期の復号で約 8.5 秒、helper のキューは約 16.5 秒分 | x86_64 の試験機で、helper のスレッドごとの CPU と、保存した IQ の再生の実時間に対する倍率を測る |
| 選局から最初の TS | x64 で 4.21〜5.11 秒 | 同じ 13ch と 17ch で測り、選局の待機の上限 (15 秒) の余裕を確かめる |
| MER | 既定の利得で 12〜14 dB | x86_64 の試験機のアンテナで測る |

検証環境 (Cortex-A72) の処理量は、x86_64 の試験機の結果を見てから試すか決めます。

## デバイスファイルの契約

| ioctl | RTL-SDR の扱い |
|---|---|
| `PTX_SET_SYSTEM_MODE` | 地デジだけを受け付ける |
| `PTX_SET_CHANNEL` | 地デジの `freq_no` 63〜102 (UHF 13〜52ch)、`slot` は 0 |
| `PTX_START_STREAMING`、`PTX_STOP_STREAMING` | 配信の開始と停止。helper と Bulk IN の受信は選局から閉じるまで続ける |
| `PTX_GET_CNR` | helper の MER を `DbToCndat()` で換算した値 |
| `PTX_GET_LOCK_STATUS` | helper の TS が 3 秒以内に届いているか |
| `PTX_ENABLE_LNB_POWER` | 0 は成功、0 以外は `ENOSYS` |
| `PTXT_SET_PARAMS`、`PTXT_TUNE` | 対応しない (`EINVAL`) |

受信機は 1 つのため、開けるのは同時に 1 つの利用者だけです。  
受信中の選局は、ほかの機種と同じく `EBUSY` を返します。  
RTL2832U の制御転送の失敗は選局の失敗 (`EIO`) として返し、デバイスファイルを失敗状態にはしません。  
WinUSB 版も選局のたびに librtlsdr で機器を開き直しており、次の選局で RTL2832U と R820T を初期化し直せるためです。

## 機器の着脱とデーモンの寿命

USB の抜去時の扱いはほかの機種と同じで、開いている利用者がいる間はデバイスファイルを残し、受信は `EIO` を返します。  
抜去を検出したら helper を止め、未完了の転送の回収を待ってから `Device` を破棄します。  
デーモンの終了の手順 (`OnShutdown`) では、helper と受信を止めてから機器を閉じ、外したカーネルのドライバーを戻します。

## 試験

### 自動試験

- 共有のクラスへ切り出した場合は、`rtlsdr_session_test` (模擬の helper の出力で、周波数の検査と換算、ロックと C/N、起動し直しの回数と待機、使い切った後の扱い、抜去の後に起動し直さないこと) を `winusb/tests/` に置き、`build.ps1` と `make -C cuse check` の両方で実行する
- 既存の `rtlsdr_protocol_test` を `make -C cuse check` でも実行する
- 模擬の helper (シェルスクリプト) で、標準入力の IQ の受け渡し、停止の期限を過ぎた場合の `SIGKILL`、キューのあふれ、異常終了の終了コードを確かめる
- `cargo build --release` と `cargo test` を Linux で実行し、Windows の helper のビルドと `cargo test` が変わらないことを `winusb/build.ps1` で確かめる

### 実機試験 (x86_64 の試験機)

試験の前後で、仮想マシンの RTL2838 (`3-1`、`3-3`) が `usbfs` のまま QEMU に使われていることを `lsusb -t` で確かめます。

1. 接続、`PX4_CUSED_RTLSDR_PORTS` に指定しない状態で RTL2838 を 1 本も開かないこと、指定した `3-4` だけを開き `dvb_usb_rtl28xxu` を外すこと
2. recpt1 で 13ch と 17ch を 60 秒ずつ受信し、選局から最初の TS までの時間、パケット数、同期・TEI・CC の異常、C/N の表示を記録する
3. 1 回開いたまま 2 つのチャンネルを交互に選局し、前のチャンネルの PAT の TSID が混ざらないことを確かめる
4. helper の処理量 (スレッドごとの CPU、デーモンの CPU、実時間に対する倍率)
5. 信号のないチャンネルの選局が `EAGAIN` で終わる時間 (「決めた事項」の 7 が A の場合)
6. helper の強制終了からの自動の起動し直しと `outage_ms`
7. 待機中と受信中の USB の抜き差し、デーモンの停止の後に `dvb_usb_rtl28xxu` が戻ること
8. 13ch の長時間受信 (2 時間を目安)

### Windows 版の回帰試験

「決めた事項」の 3 で共有のクラスへ切り出す場合と helper を変える場合は、切り出しの前後で次を比べます。  
試験には、運用中の DriverHost_PX4 を止めずに済む試験用のビルド (`/p:RtlSdrIsolatedTest=true`) を使います。

- `winusb/build.ps1` の x86・x64 のビルドと自動試験 (`rtlsdr_protocol_test`、DSP の試験、切り出しで加える試験)
- `rtlsdr_bondriver_live.ps1 -IsolatedHost` の 13ch の受信を x64 と x86 で
- `rtlsdr_bondriver_live.ps1 -AlternateChannel 1 -Cycles 4` の交互選局 (x64)
- helper を変えた場合は `rtlsdr_tuning_regression.py` で、保存した 13ch の IQ の再生の TS が変更の前後でバイト単位で一致すること

## 実装

### 共有コードの切り出し

「決めた事項」の 3 のとおり、WinUSB 版の `Receiver` から OS に依存しない処理を [`rtlsdr/session.{hpp,cpp}`](../winusb/src/DriverHost_PX4/rtlsdr/session.hpp) の `rtlsdr::Session` へ移しました。  
`Session` は、周波数の検査と換算 (`HelperFrequency`)、helper の出力の解釈、ロックの判定、監視のスレッドと起動し直し (`RecoveryBudget`、抜去の確認、`helper restart` と `outage_ms` のログ)、使い切った後の扱いを持ちます。  
OS ごとの処理は `HelperPort` (起動、標準出力の読み取り、停止の要求と期限付きの待機、強制終了、解放、機器の有無) で受け取ります。

| `HelperPort` の実装 | 内容 |
|---|---|
| WinUSB 版 `rtlsdr/rtl_device.cpp` の `Receiver` | 従来の `CreateProcessW`、job object、`PeekNamedPipe`、デバイスインターフェースのパスの確認をそのまま移した |
| Linux 版 `cuse/src/rtlsdr_device.cpp` の `RtlSdrDevice` | librtlsdr による設定、15 本の非同期 Bulk 転送、helper の子プロセス (`cuse/src/rtlsdr_helper.cpp` の `RtlSdrHelperProcess`) を一組で起動と停止する |

切り出しで変えた動作は次の 2 点です。

- ロックの判定の時刻を、Windows の `GetTickCount64()` から共有の `NowMs()` (`steady_clock`) へ変えた (PX-S1UD の切り出しと同じ)
- 配信の満杯の記録 (`StreamOverflowLog`) を、helper の起動ごとの番号で作り直す形にした。従来も helper の読み取りごとに作り直しており、記録の区切りは変わらない

`rtlsdr_session_test` (`winusb/tests/`、`build.ps1` と `make -C cuse check` の両方で実行) は、模擬の helper で、周波数の換算、ロックと MER、停止の手順、予期しない終了と形式の異常と helper のエラーからの起動し直し、8 回の起動し直しの使い切りと選局による解除、抜去の後に起動し直さないことを確かめます。

### helper

`rtlsdr_frontend` に `--stdin` を加え、`rtlsdr.dll` を読み込む部分 (`kernel32` の宣言、librtlsdr の型、`capture`) を `#[cfg(windows)]` で分けました。  
`--stdin` の入力は 2 バイト単位に揃えて復調器へ渡し、helper のキューが満杯なら失敗として終わります (ライブ入力の `on_iq` と同じ扱い)。  
Windows 版の動作 (USB の取得と、標準入力の 1 バイトによる停止の要求) は変えていません。  
`isdbt-dsp` は変えていません。

### Linux 版

`RtlSdrDevice` は、`InitModel` で librtlsdr の `rtlsdr_open_handle` を使ってチューナーを確かめ、R820T でなければ機器を手放します。  
選局のたびに WinUSB 版の helper と同じ順に RTL2832U と R820T を開き直して設定し、helper を起動してから Bulk IN の受信を始めます。  
Bulk IN の完了の通知は、I/Q を helper の標準入力のキュー (256 KiB × 64 個) へ複写して同じ転送を出し直すだけで、キューが満杯の場合と転送の失敗は `Session` へ失敗として伝え、helper と受信を起動し直させます。  
`PTX_SET_CHANNEL` は `Session::Locked()` (3 秒以内の TS) を最長 15 秒待ちます。  
`main.cpp` の機種の表には、開く前に機器を選ぶ関数 (`RtlSdrPortSelected`) の欄を加え、ほかの機種は従来どおりすべて開きます。

librtlsdr は RTL-SDR Blog V1.4.0 のソースを `cuse/third_party/rtl-sdr-blog/` へ置き、`rtlsdr_open_handle` を加えました (変更点は [README](../cuse/third_party/rtl-sdr-blog/README.md))。  
helper は `make -C cuse rtlsdr` で作り、`make -C cuse install-rtlsdr` で `$(PREFIX)/lib/px4-cused/` へ置きます。

### ビルドと自動試験

- WinUSB 版: 試験用のビルド (`RtlSdrIsolatedTest=true`) の DriverHost_PX4 と BonDriver_PX4、`rtlsdr_protocol_test`、`receiver_tune_completion_test`、`rtlsdr_session_test` を x86・x64 でビルドして実行し、すべて成功しました。helper は `build-bondriver.ps1` の `cargo test` (41 件と 5 件) と release のビルドが x86・x64 で成功しました
- Linux 版: x86_64 の試験機 (Debian 13 の GCC 14) で `make -C cuse` に新たな警告はなく (既存の torne の生成コードの警告だけ)、`make -C cuse check` は `rtlsdr_protocol_test`、`rtlsdr_session_test`、`rtlsdr_helper_test` を含めてすべて成功しました
- helper: x86_64 の試験機の Debian の cargo 1.85 で `make -C cuse rtlsdr` と `cargo test` (41 件と 5 件) が成功しました

### Windows 版の回帰試験の結果

Windows 11 x64 の PC の USB ポートへ同じ RTL2838UHIDIR をつなぎ、切り出しの前 (2026-11-22 にビルドした試験用の DriverHost_PX4、BonDriver_PX4、helper) と後で同じ試験を行いました。  
運用中の DriverHost_PX4 は止めず、分離した通信名の試験用のビルドを `rtlsdr_bondriver_live.ps1 -IsolatedHost` で動かしました。  
この PC では機器全体に libwdi (Zadig) の WinUSB のドライバーが入っていて DriverHost_PX4 が機器を見つけられなかったため、利用者がそのドライバーを外し、配布物の `Install-Driver.ps1` で interface 0 だけを WinUSB にしてから試しました。

| 試験 | 切り出しの前 | 切り出しの後 |
|---|---|---|
| x64、13ch を 30 秒 × 2 周期 (`AsyncTune=true`) | 8,800 と 8,832 パケット、同期・TEI・CC・不正な PAT・空受信 0、最初の TS 5,891 ms と 4,813 ms、信号レベル (MER) 14.33〜14.42 dB | 8,832 と 8,832 パケット、異常 0、最初の TS 7,188 ms と 4,922 ms、14.35〜14.44 dB |
| x64、13ch と 14ch の交互選局を 15 秒 × 4 周期 | 4,608〜4,640 パケット、異常 0、旧局の PAT の混入 0、最初の TS 4,922〜5,500 ms | 4,544〜4,608 パケット、異常 0、混入 0、最初の TS 5,110〜5,359 ms |
| x86、13ch を 30 秒 × 2 周期 | 8,896 と 8,896 パケット、異常 0、最初の TS 7,688 ms と 5,312 ms | 8,928 と 8,864 パケット、異常 0、最初の TS 8,906 ms と 5,125 ms |

各周期の 1 回目の最初の TS が長いのは、DriverHost_PX4 の起動の直後の最初の選局のためで、前後のどちらにも現れました。

helper の変更 (`--stdin` と `#[cfg(windows)]`) は、同じ PC で収録した 13ch の 30 秒の I/Q (SHA-256 `95F770D3…`) を `--replay` で再生して比べました。  
切り出しの前と後の helper の TS は、x64・x86 とも 8,111 パケットで SHA-256 (`d6be35b9…`) が一致しました。  
同じ I/Q を x86_64 の試験機の Linux 版の helper の `--stdin` へ実時間より少し速く (2.1 MB/s) 流した TS も、8,111 パケットで同じ SHA-256 でした。  
`rtlsdr_tuning_regression.py` (内部で `rtlsdr_frontend_replay_test.py` を使う) は、状態のフレームの payload を 0 バイトに限る検査で、MER を載せる 2026-11-12 以降の helper では切り出しの前の helper でも失敗していました。  
`rtlsdr_frontend_replay_test.py` の検査を `rtlsdr/protocol.hpp` と同じ範囲 (4 の倍数で 64 バイトまで、先頭の MER は 10000 以下) に直し、同じ I/Q の開始位置の 4 点で、切り出しの前と後の helper の TS が x64・x86 ともバイト単位で一致することを確かめました。

## Linux 版の実機確認

x86_64 の試験機の `3-4` へ直接つないだ RTL2838UHIDIR (R820T、シリアル番号 `00000001`) で、地上波の信号を入力して確かめました。  
デーモンは、`px4-cused.service` と同じ制限 (`CapabilityBoundingSet=` 空、`NoNewPrivileges=yes`、`DevicePolicy=closed` と `/dev/cuse`・USB 機器だけの許可、`ProtectSystem=strict`) を `systemd-run` で付け、`PX4_CUSED_RTLSDR_PORTS=3-4` で動かしました。  
導入前の helper は `PX4_CUSED_LIBDIR` で指定し、置き場所がホームディレクトリーの下のため `ProtectHome` だけは付けていません。  
試験の間、仮想マシンの RTL2838 (`3-1`、`3-3`) は `usbfs` のまま QEMU に使われ続けました。

| 試験 | 結果 |
|---|---|
| 接続 | `3-4` だけを開いて `dvb_usb_rtl28xxu` を外し、R820T を判定して `/dev/rtlsdrvideo0` を作った。`3-1` と `3-3` はログに `not listed in PX4_CUSED_RTLSDR_PORTS; left untouched` を残して開かなかった |
| recpt1 で 13ch を 60 秒 | 16,963 パケット、同期・TEI・CC の異常 0、C/N 表示 13.07 dB。選局 (最初の TS まで) 3,688 ms、helper の最初の TS は起動から 2.60 秒 |
| recpt1 で 14ch と 17ch を 30 秒ずつ | 8,629 と 8,737 パケット、異常 0、C/N 表示 12.24 dB と 12.97 dB。選局 3,960 ms と 4,031 ms |
| 1 回開いたまま 13ch と 14ch を 10 秒ずつ 5 周期 (10 回の選局) | 31,020 パケット、同期・TEI・CC の異常 0。各区間の PAT の TSID は 0x7c11 と 0x7c12 (PX-S1UD の地デジと同じ) で、別の TSID の PAT は 0。選局 3,719〜4,185 ms |
| 信号のないチャンネル (40ch) | 15,004 ms で `EAGAIN` |
| 受信の 20 秒目に helper を `SIGKILL` | 同じ周波数で起動し直した (`exit_code=137`)。受信の途切れは `outage_ms=3963`、60 秒の TS は 16,600 パケットで、CC の飛び 8 (PID ごとに 1 回)、同期・TEI の異常 0 |
| 受信中にデーモンを停止 | 260 ms で止まり、helper は残らなかった。`dvb_usb_rtl28xxu` が戻り、`/dev/dvb/adapter0` と `/dev/swradio0` が作り直された。停止までの TS は異常 0 |
| recpt1 で 13ch を 484 秒 (2 時間の予定を利用者の判断で途中で止めた) | 136,346 パケット、同期・TEI・CC の異常 0、helper の起動し直し 0 |
| 待機中の USB の抜き差し | 抜去を検出してデバイスファイルを消し、挿し直した機器から再び `dvb_usb_rtl28xxu` を外して `/dev/rtlsdrvideo0` を作った |
| 受信中の USB の抜き差し (修正の後) | 抜去までの 5,220 パケットは異常 0。`read` は `EIO`、helper は止まった。挿し直した機器で 13ch を 15 秒受信し、4,456 パケット、異常 0 |

`ptx_switch` (1 回開いたまま選局を繰り返す試験用のプログラム) は、各区間の最初のパケットが PAT であることも条件にしていて、RTL-SDR ではすべての区間がこの条件だけで FAIL になりました。  
helper の TS はワンセグの任意のパケット (PMT や ES) から始まり、WinUSB 版も同じです。  
前のチャンネルの混入は TSID と CC で確かめており、試験の条件は変えていません。

受信中の抜去の最初の試験では、`read` が `EIO` ではなく 0 (EOF) を返しました。  
`VideoNode::Remove` は受信を止めてから `EIO` を設定し、その間に `StopStream` の戻り値 (0) で待機中の `read` に EOF を返します。  
RTL-SDR はこの間の `OnRemove` で helper を止めるため数十 ms かかり、利用者の `read` がこの間に入りました。  
抜去の後の `StopStream` が `EIO` を返すように直し、上の表の再試験で `EIO` になることを確かめました。  
PX-S1UD の `StopStream` も抜去の後に 0 を返していたため、同じく `EIO` を返すように直しました。  
PX-S1UD は `OnRemove` で helper と受信を止める時間が RTL-SDR より短い可能性があり、EOF を実機では再現しておらず、直した後の実機の抜去も確かめていません。

### 処理量

| 項目 | 値 |
|---|---|
| helper の復調のスレッドの CPU (受信の 30 秒後から 10 秒) | 1 コアの 65.6〜65.7% |
| helper の標準入力を読むスレッド | 0% |
| デーモンの CPU | 1 コアの 0.3〜0.4% |
| CPU のクロック (4 コアの平均) | 2,900 MHz |
| 試験機の負荷 | 4 台の仮想マシンが動き、load average は 1.0〜2.2 |

Intel N150 の 1 コアで、復調は実時間の約 0.66 倍で、ほかの仮想マシンが動く状態でも余裕があります。  
検証環境 (Cortex-A72) の処理量は確かめていません。

## 決めた事項

利用者の判断を仰いだ事項と、その結果は次のとおりです。

| 事項 | 選択肢 | 設計者の推奨 | 決定 |
|---|---|---|---|
| 1. 受信のデバイスファイルの名前 | `rtlsdrvideoN` / `rtl2838videoN` | `rtlsdrvideoN` (WinUSB 版の配布名 `BonDriver_RTLSDR` と同じ呼び方) | `rtlsdrvideoN` (2026-11-22) |
| 2. USB を持つプロセスと復調の動かし方 | A. デーモンが USB を持ち、helper は標準入力の IQ を復調するだけ / B. WinUSB 版と同じく helper が librtlsdr で USB を持つ | A (接続位置で選んでから開け、仮想マシンの機器を開かない) | A (2026-11-22) |
| 3. WinUSB 版との共有の範囲と順序 | A. `protocol.hpp` だけを共有し、監視と起動し直しは Linux 版で書く / B. 監視と起動し直しを `rtlsdr::Session` へ切り出し、Windows の回帰試験を済ませてから Linux 版を作る | B (実機で決めた起動し直しの条件を OS ごとに複製しない) | B (2026-11-22) |
| 4. 機器の選び方 | A. 接続位置 (`3-4`) を環境変数で指定し、開く前に選ぶ / B. シリアル番号で指定する / C. すべての RTL2838 を開く | A (シリアル番号は `3-3` と `3-4` で同じで、読むには開く必要がある) | A (2026-11-22) |
| 5. カーネルの `dvb_usb_rtl28xxu` との関係 | A. 指定した機器を開くときに外し、閉じるときに戻す / B. blacklist する設定を配布する | A (ほかの RTL2838 とホストの設定を変えない) | A (2026-11-22) |
| 6. librtlsdr のソースの入手 | A. RTL-SDR Blog V1.4.0 の必要なファイルをリポジトリへ置く / B. `make` で SHA-256 を固定した ZIP を取得する | A (オフラインでビルドでき、上流との差分を確かめられる) | A (2026-11-22) |
| 7. `PTX_SET_CHANNEL` の完了 | A. 最初の TS を最長 15 秒待つ / B. helper の起動で返す | A (選局の失敗を利用者が検出できる) | A (2026-11-22) |

設計者が決めた次の事項は、異論があれば見直します。

- UHF 13〜52ch だけを受け付け、`slot` が 0 でない要求を断る
- R820T 以外のチューナーは開いた後に断る
- C/N は MER を `DbToCndat()` で換算し、MER の前は下限を返す
- 利得は環境変数 `PX4_CUSED_RTLSDR_GAIN` で変えられ、既定は WinUSB 版と同じ 297
- helper の標準エラー出力は journal へ送る
- 最初の実機試験は x86_64 の試験機で行う

## 未確認の条件

- 2 時間を超える受信 (484 秒で止めた)
- 検証環境 (Raspberry Pi 4) での受信と処理量
- 13ch・14ch・17ch 以外のチャンネルと、弱電界での初回の整列評価の失敗の後の再蓄積
- USB の受信の失敗 (抜去以外) と helper の標準入力のキューのあふれからの起動し直し (自動試験だけで確かめた)
- R820T 以外のチューナーを断る処理 (R820T の機器だけで試した)
