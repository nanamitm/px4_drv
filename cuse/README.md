# px4-cused (PIX-DT295、Sony torne CECH-ZD1J、I-O DATA GV-MVP/XZ3、PLEX PX-BCUD、PLEX PX-S1UD、RTL-SDR、MSi2500 の Linux 版)

PIXELA PIX-DT295 (USB ID `06b8:103b`)、Sony torne CECH-ZD1J (USB ID `054c:04b2`)、I-O DATA GV-MVP/XZ3 (USB ID `04bb:0530`)、PLEX PX-BCUD (USB ID `3275:0085`)、PLEX PX-S1UD (USB ID `3275:0080`)、RTL-SDR (RTL2838UHIDIR、USB ID `0bda:2838`、チューナー R820T) のワンセグ、SDRplay RSP1 の互換機 (Mirics MSi2500 + MSi001、USB ID `1df7:2500`) のフルセグと J.83B を Linux で使うためのユーザー空間のデーモンです。  
libusb で機器を動かし、CUSE (Character device in Userspace) で次のデバイスファイルを作ります。

| デバイスファイル | 機種 | 利用者 | 契約 |
|---|---|---|---|
| `/dev/dt295videoN` | PIX-DT295 (地デジ・BS・110度CS) | recpt1、BonDriverProxy_Linux、Mirakurun など | カーネルモジュールの `ptx_chrdev` と同じ PTX の ioctl、`read`、`poll` |
| `/dev/tornevideoN` | CECH-ZD1J (地デジ UHF 13〜62ch) | 同上 | 同上 |
| `/dev/xz3videoN` | GV-MVP/XZ3 (地デジ・BS・110度CS) | 同上 | 同上 |
| `/dev/pxbcudvideoN` | PX-BCUD (BS・110度CS) | 同上 | 同上 |
| `/dev/pxs1udvideoN` | PX-S1UD (地デジ、J.83B は実験的) | 同上 | 同上。J.83B はカーネルモジュールと同じ拡張 ioctl (`PTXT_SET_PARAMS`、`PTXT_TUNE`) |
| `/dev/rtlsdrvideoN` | RTL-SDR (地デジのワンセグ、UHF 13〜52ch) | 同上 | 同上。使う機器を接続位置で指定する (「RTL-SDR の注意」) |
| `/dev/msi2500videoN` | RSP1 互換機 (地デジのフルセグ、UHF 13〜52ch、J.83B) | 同上 | 同上。J.83B は PX-S1UD と同じ拡張 ioctl。使う機器を接続位置で指定する (「MSi2500 の注意」) |
| `/dev/px4card/<ID>` | 各機種の内蔵カードリーダー | px4-drv-pcsc (pcscd の IFD ハンドラー) | `include/px4_card_ioctl.h` |

設計の理由、契約の詳細、実機での測定は次の文書にまとめています。

- PIX-DT295: [docs/20261120_dt295_linux_cuse_design.md](../docs/20261120_dt295_linux_cuse_design.md)
- CECH-ZD1J と、デーモンを機種共通にした構成: [docs/20261121_torne_linux_cuse_design.md](../docs/20261121_torne_linux_cuse_design.md)
- GV-MVP/XZ3: [docs/20261121_gv_mvp_xz3_linux_cuse_design.md](../docs/20261121_gv_mvp_xz3_linux_cuse_design.md)
- PX-BCUD: [docs/20261122_px_bcud_linux_cuse_design.md](../docs/20261122_px_bcud_linux_cuse_design.md)
- PX-S1UD: [docs/20261122_px_s1ud_linux_cuse_design.md](../docs/20261122_px_s1ud_linux_cuse_design.md)
- RTL-SDR: [docs/20261122_rtlsdr_linux_cuse_design.md](../docs/20261122_rtlsdr_linux_cuse_design.md)
- MSi2500: [docs/20261123_msi2500_linux_cuse_design.md](../docs/20261123_msi2500_linux_cuse_design.md)

制御転送、PIX-DT295 の SERVICE のループ、torne の初期化と選局、GV-MVP/XZ3 の中継プログラムと TS の取り出し、PX-BCUD の初期化と選局と TS の組み立て、PX-S1UD の firmware の切り替えと選局、RTL-SDR と MSi2500 の helper の監視と起動し直し、カードの操作は、WinUSB 版 (`winusb/src/DriverHost_PX4/dt295/`、`torne/`、`xz3/`、`pxbcud/`、`siano/`、`rtlsdr/`) と同じコードを使います。

## ビルド

libusb-1.0 と libfuse3 の開発用パッケージが必要です。  
カーネルモジュールと異なり、カーネルのヘッダーと DKMS は不要です。

```bash
sudo apt install build-essential pkg-config libusb-1.0-0-dev libfuse3-dev
make -C cuse
make -C cuse check
```

`make check` は USB と CUSE を使わない自動試験で、次を確かめます。

- WinUSB 版と共有する PIX-DT295 の制御経路、選局と C/N の換算、SHA-256 と組み込みファームウェア
- torne の共有セッション (`torne_session_test`)
- torne の復調器の生成コードが、リポジトリにある Go の正解の記録と同じ I²C の要求列になること
- GV-MVP/XZ3 のカード UART の手順と、TS の取り出しの境界の処理 (`xz3_card_test`、`xz3_stream_test`)
- GV-MVP/XZ3 の受信の開始と停止で、EP81 に残った前の受信の末尾を次の受信へ渡さないこと (`xz3_session_test`)
- PX-BCUD の初期化と選局の要求列、TS の境界の獲得、前のチャンネルの TS の除外、自動復旧の回数 (`pxbcud_frontend_test`) と、isochronous のパケットから TS を組み立てる処理 (`pxbcud_stream_test`)
- PX-S1UD の firmware の転送と選局と統計の要求 (`siano_frontend_test`)、地デジと J.83B の firmware の切り替えの順序とメッセージの振り分け (`siano_session_test`)、J.83B の helper の子プロセスの起動と停止 (`iq_process_test`、模擬の helper のシェルスクリプトを使う)
- RTL-SDR の helper の出力の形式 (`rtlsdr_protocol_test`)、周波数の換算とロックと helper の起動し直しの回数 (`rtlsdr_session_test`)、helper の子プロセスの標準入力の I/Q と停止とキューのあふれ (`rtlsdr_helper_test`、模擬の helper のシェルスクリプトを使う)

PIX-DT295 のファームウェアと GV-MVP/XZ3 の中継プログラムは実行ファイルへ埋め込み、torne と PX-BCUD はメーカーのファームウェアを使わないため、別のファイルを配置する手順はありません。  
PX-S1UD の地デジは、linux-firmware の `isdbt_rio.inp` (`/lib/firmware`) を使います。Debian 系では `firmware-siano` パッケージに含まれます。

### PX-S1UD の J.83B (実験的)

PX-S1UD の J.83B は、復調前の IQ を送る firmware (`s1ud_j83b.inp`) を機器へ読み込み、ホストの helper (`msi2500_frontend serve-s1ud`) で復調します。  
どちらもこのリポジトリのソースから作り、地デジだけを使う場合は不要です。helper は GPL-3.0-or-later のため、デーモン (GPL-2.0-only) とは別のプロセスとして動きます。

```bash
sudo apt install cargo binutils-arc-linux-gnu python3
make -C cuse j83b
sudo make -C cuse install-j83b
```

`install-j83b` は helper と firmware を `/usr/local/lib/px4-cused/` へ置きます。  
helper の処理量は大きく、Ryzen 9 5900X の 1 スレッドで実時間の約 0.64 倍 (前処理と復調) と別スレッドの約 0.45 倍 (誤り訂正) です。実時間に間に合わない CPU では TS が欠けます。  
Intel N150 (ほかの仮想マシンが動く状態) では前処理と復調のスレッドが 1 コアを使い切り、約 60 秒を超えるとデーモンと helper の間のキューがあふれて TS が欠けました。Raspberry Pi 4 は対象外です。ログの helper の終了の行の `queue_dropped_blocks` が 0 であることを確かめてください。

### RTL-SDR のワンセグ

RTL-SDR は復調器を持たず、デーモンが USB から受けた I/Q をホストの helper (`rtlsdr_frontend --stdin`) で復調します。  
helper は WinUSB 版と同じソース (`winusb/tools/rtlsdr_frontend`、GPL-3.0-or-later) から作り、デーモン (GPL-2.0-only) とは別のプロセスとして動きます。  
RTL2832U と R820T の設定には、RTL-SDR Blog V1.4.0 の librtlsdr (`third_party/rtl-sdr-blog`、GPL-2.0-or-later、WinUSB 版の `rtlsdr.dll` と同じ版) をデーモンへ組み込んで使います。

```bash
sudo apt install cargo
make -C cuse rtlsdr
sudo make -C cuse install-rtlsdr
```

`install-rtlsdr` は helper を `/usr/local/lib/px4-cused/` へ置きます。  
Intel N150 では、helper の復調のスレッドが 1 コアの約 66% を使いました (設計文書の「Linux 版の実機確認」)。

### MSi2500 のフルセグと J.83B

RSP1 互換機は復調器を持たず、デーモンが USB の等時転送で受けた生ブロックを、ホストの helper (`msi2500_frontend serve --stdin`) で復調します。  
helper は WinUSB 版と同じソース (`winusb/tools/msi2500_frontend`、GPL-3.0-or-later) から作り、PX-S1UD の J.83B と同じ実行ファイルです (MSi2500 には firmware は不要です)。  
MSi2500 と MSi001 のレジスターの設定値も helper が計算し (`msi2500_frontend ops`)、デーモンはそれを送るだけです。

```bash
sudo apt install cargo
make -C cuse msi2500
sudo make -C cuse install-msi2500
```

`install-msi2500` は helper を `/usr/local/lib/px4-cused/` へ置きます (`install-j83b` で置いた場合は同じファイルです)。  
helper はフルセグの復調と誤り訂正に、Intel N150 で約 1.2 コアを使います。Raspberry Pi 4 は処理量が足りないため対象外です。

## 導入

```bash
sudo make -C cuse install
sudo udevadm control --reload
sudo systemctl daemon-reload
sudo systemctl enable --now px4-cused
```

`/dev/cuse` を開けるプロセスは任意の名前でデバイスファイルを作れるため、デーモンは root で動かし、systemd のユニットで特権 (capability) を外して、開けるデバイスを `/dev/cuse` と USB 機器に限ります。  
作られたデバイスファイルの権限は `dist/90-px4-cused.rules` で設定し、受信は `video` グループ、カードは `pcscd` グループに許可します。

内蔵カードリーダーを pcscd から使うには、px4-drv-pcsc ([pcsc/README.md](../pcsc/README.md)) も導入します。  
IFD ハンドラーは、デーモンが書く `/run/px4-cused/readers` で USB 機器とカードのデバイスファイルを対応させるため、pcscd のホットプラグで認識されます。  
ユニットではこのディレクトリを systemd (`RuntimeDirectory=`) が作り、停止時に消します。`systemctl stop` の後にデーモンを手動で起動した場合は、デーモンが `/run/px4-cused` を 0755 で作ります。対応表を書けない場合は、ログにパスと理由 (`reader map: cannot ...`) が残ります。

### px4-dt295d からの移行

以前の名前の `px4-dt295d` を導入した環境では、同じ機器を開くため、先に止めて外します。  
新しいユニットは `px4-dt295d.service` と同時には動かない設定です。

```bash
sudo systemctl disable --now px4-dt295d
sudo rm -f /usr/local/sbin/px4-dt295d /lib/systemd/system/px4-dt295d.service /lib/udev/rules.d/90-px4-dt295.rules
```

px4-drv-pcsc は新旧どちらの対応表 (`/run/px4-cused/readers`、`/run/px4-dt295/readers`) も読むため、導入の順序は問いません。  
PIX-DT295 の受信とカードのデバイスファイルの名前と ID は変わりません。

### torne_drv と一緒に使う場合

カーネルモジュールの torne_drv (mihalinn/torne_drv) を導入した環境では、そのモジュールが CECH-ZD1J の USB の interface を先に取るため、デーモンは機器を開けず、ログに `claim failed` を残します。  
受信のデバイスファイルの名前 (`tornevideoN`) も重なるため、CECH-ZD1J にはどちらか一方だけを使ってください。

## 動作

機器を接続すると、受信のデバイスファイルを機種ごとに空いている最小の番号で作り、カードのデバイスファイルを ID で作ります。

- PIX-DT295 の ID は USB のシリアル番号から求め、Windows 版のリーダー名に表示される ID と同じです。
- CECH-ZD1J と GV-MVP/XZ3 はシリアル番号を持たないため、ID は USB の接続位置 (バス番号とポートの経路) から求めます。同じポートへ挿し直すと同じ ID になり、別のポートへ挿すと変わります。Windows 版の ID とは一致しません。

受信のデバイスファイルを閉じるたびに、PIX-DT295 は起動前の状態へ戻し、CECH-ZD1J は復調器を休止させます。  
CECH-ZD1J のブリッジは、受信とカードのどちらかが開いている間は起動したままです。  
GV-MVP/XZ3 は、最初の選局で機器の RAM へ中継プログラムを入れ、受信のデバイスファイルを閉じるときとデーモンを止めるときに元の内容へ戻します。

### PX-BCUD の注意

- Linux の本体の `em28xx` ドライバーも PX-BCUD に対応しており、機器をつなぐと先に interface を取って DVB のデバイス (`/dev/dvb/adapterN`) を作ります。デーモンは機器を開くときに `em28xx` を外し (ログの `kernel driver detached`)、デーモンを止めるときに戻します (`kernel driver reattached`)。DVB API で使う場合は、デーモンを止めてください。
- TS は isochronous 転送で受け取ります。受信の処理が遅れると TS が欠けるため、ログの `TS stopped` の `iso_errors` と `max_completion_gap_us` (転送の完了の間隔の最大。1 本は 8 ms) を確かめてください。
- 転送の失敗と、TS が 5 秒届かない場合は、同じチャンネルのまま受信を再開します (ログの `auto recovery begin`)。再開が 8 回続けて実らない場合と、選んだ TS の PAT が 5 秒届かない場合は、`read` が `EIO` を返します。この動作は、`make -C cuse fault` で作る試験用のデーモン (`build/px4-cused-fault`、導入の対象外) に模擬の故障を入れて確かめられます (設計文書の「模擬の故障による自動復旧の確認」)。

### PX-S1UD の注意

- Linux の本体の `smsusb` ドライバーも PX-S1UD に対応しており (カーネルの構成による)、機器をつなぐと先に interface を取って DVB のデバイスを作ります。デーモンは PX-BCUD と同じく、機器を開くときに `smsusb` を外し、デーモンを止めるときに戻します。J.83B 用の firmware で受信していた場合は、地デジの firmware へ戻してから返します。
- J.83B は `PTXT_SET_PARAMS` で放送方式 `PTX_J83B_SYSTEM` と中心周波数 (kHz) を指定し、`PTXT_TUNE` で選局します ([docs/isdbc_tuning.md](../docs/isdbc_tuning.md) の「Linux の ioctl」)。追加パラメーターは受け付けません。ロックは helper の TS が届いたことで判定し、最大 2 秒待ちます。helper か firmware を導入していない場合は `ENOENT` を返し、地デジの受信には影響しません。
- `PTX_GET_CNR` は、地デジは firmware の SNR、J.83B は helper の復調器の MER を、recpt1 などの地デジの換算式で同じ dB になる値で返します。
- 制御の応答が届かない場合と形式の異常の後は、WinUSB 版と同じく自動では復旧せず、デバイスファイルを閉じるまで `EIO` を返します。helper だけが異常終了した場合は、次の選局で起動し直します。

### RTL-SDR の注意

- RTL2838 は SDR の用途に広く使われ、多くの製品が同じシリアル番号 (`00000001`) を持つため、デーモンは環境変数 `PX4_CUSED_RTLSDR_PORTS` に書いた接続位置の機器だけを開きます。指定がなければ RTL2838 を 1 本も開かず、指定のない機器には制御転送も送りません (ログの `not listed in PX4_CUSED_RTLSDR_PORTS; left untouched`)。
- 接続位置は sysfs と同じ「バス番号-ポートの経路」です。`lsusb -t` の `Bus 003` の `Port 004` なら `3-4`、ハブの先は `3-2.1` のように書き、複数はカンマで区切ります。`/sys/bus/usb/devices/3-4/idProduct` が `2838` であることで確かめられます。別のポートへ挿し替えた場合は、指定も変えてください。

  ```bash
  sudo systemctl edit px4-cused
  # [Service]
  # Environment=PX4_CUSED_RTLSDR_PORTS=3-4
  sudo systemctl restart px4-cused
  ```

- Linux の本体の `dvb_usb_rtl28xxu` ドライバーも RTL2838 に対応しており、DVB-T と SDR (`/dev/swradioN`) のデバイスを作ります。デーモンは指定した機器だけ、開くときにこのドライバーを外し、デーモンを止めるときに戻します。
- チューナーが R820T (R820T2 を含む) でない機器は、確かめていないため開いた後に断ります。
- 選局は UHF 13〜52ch (`freq_no` 63〜102、`slot` は 0) だけで、最初の TS が届くまで最大 15 秒待ちます。届かない場合は `EAGAIN` を返します。選局から最初の TS までは約 4 秒かかります。
- `PTX_GET_CNR` は、helper の復調器の MER を recpt1 などの地デジの換算式で同じ dB になる値で返します。MER は RF の C/N とは異なり、入力が過大だと下がります。利得は環境変数 `PX4_CUSED_RTLSDR_GAIN` (0.1 dB 単位、既定 297) で変えられます。
- helper が予期せず終わった場合と USB の受信が失敗した場合は、同じチャンネルのまま helper と受信を起動し直します (ログの `helper restart`、受信が戻ると `outage_ms`)。欠けた区間は CC の飛びとして現れます。起動し直しが 8 回続けて実らない場合は、選局し直すまでロックなしを返します。

### MSi2500 の注意

- MSi2500 は汎用の SDR で、シリアル番号を返さない個体もあるため、RTL-SDR と同じく環境変数 `PX4_CUSED_MSI2500_PORTS` に書いた接続位置の機器だけを開きます。書き方は `PX4_CUSED_RTLSDR_PORTS` と同じです (`/sys/bus/usb/devices/3-4/idProduct` が `2500` であることで確かめられます)。
- Linux の本体の `msi2500` ドライバー (V4L2 の SDR、`/dev/swradioN`) も対応しています。デーモンは指定した機器だけ、開くときにこのドライバーを外し、デーモンを止めるときに戻します。
- 地デジは UHF 13〜52ch (`freq_no` 63〜102、`slot` は 0) です。J.83B は PX-S1UD と同じく `PTXT_SET_PARAMS` で放送方式 `PTX_J83B_SYSTEM` と中心周波数 (kHz、90,000〜775,000) を指定し、`PTXT_TUNE` で選局します。どちらも最初の TS が届くまで最大 15 秒待ち、届かない場合は `EAGAIN` を返します。地デジは約 2 秒、J.83B は 1 秒未満で TS が届きます。
- `PTX_GET_CNR` は helper の復調器の MER を、recpt1 などの地デジの換算式で同じ dB になる値で返します。利得は環境変数 `PX4_CUSED_MSI2500_GAIN` (地デジ、既定 40) と `PX4_CUSED_MSI2500_GAIN_J83B` (J.83B、既定 46) で変えられます (MSi001 の合計の dB、0〜102)。
- USB の等時転送の失敗したパケットは捨て、helper が欠落として扱います。ログの `usb transfers=... bad_packets=...` を確かめてください。helper が予期せず終わった場合と、データが 3 秒届かない場合、転送の失敗が続いた場合は、RTL-SDR と同じく同じチャンネルのまま起動し直します。
- 等時転送は 1 マイクロフレームに 3 KiB の帯域を予約します。同じ USB のバスの等時転送の機器 (PX-BCUD など) と同時に使う場合の帯域は確かめていません。

### GV-MVP/XZ3 の注意

- デーモンを強制終了 (SIGKILL) すると RAM を戻せないため、次のデーモンは RAM の照合に失敗し、USB を挿し直すまで受信できません。`systemctl restart` と `stop` は RAM を戻してから終わります。
- WinUSB 版では、USB 2.0 ハブを経由すると TS の取り出しが受信の流量に追いつきませんでした。PC の USB ポートへ直接つなぎ、ログの `ring high` (機器内のリングの残りが少ない) を確かめてください。
- GV-MVP/XZ3 は電源を入れた後 (USB を挿した後)、TS の取り込みが壊れた状態で始まることがあります (WinUSB 版でも同じ)。デーモンはこの状態を検出すると、壊れた TS を渡さずに受信を失敗にし (`read` が `EIO`)、ログに `TS capture is in a bad state after power-on ... reconnect USB` または `no TS input ... reconnect USB` を残します。この場合は、正常に受信できるまで USB を挿し直してください。原因と自動の回復は分かっていません (設計文書の「電源投入後の壊れた状態」)。
- 内蔵カードリーダーの通信は TS の取り出しと同じ経路を時分割で使います。WinUSB 版では 110度CS の受信開始の直後に TS の欠落を確認しているため、110度CS の録画には外付けのカードリーダーを検討してください。

機器を抜いたときは、開いている利用者がいる間はデバイスファイルを残し、受信は `EIO`、カードは `ENODEV` を返します。  
全員が閉じた時点でデバイスファイルを消します。  
同じ機器を挿し直した場合、pcscd が古いカードのデバイスファイルを閉じるまで、新しいカードのデバイスファイルの作成を保留します。

デーモンのログは journald に記録されます (`journalctl -u px4-cused`)。  
受信の終了時には、TS の計数 (同期、TEI、CC の飛び) を記録し、PIX-DT295 は SERVICE の回数と最大所要時間、CECH-ZD1J は最初の TS までの時間と Bulk IN の完了の最大の間隔も記録します。

## 対応していないもの

- アンテナ給電 (`PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS`)。どの機種も対応していないためです。PIX-DT295、GV-MVP/XZ3、PX-BCUD で BS・110度CS を受信するには外部からの給電が必要です。
- PX-BCUD の地デジ。衛星の復調器とチューナーだけを持つため、地デジの選局は `EINVAL` を返します。
- RTL-SDR のフルセグ、VHF と CATV 帯、BS・110度CS、R820T 以外のチューナー。選局は `EINVAL` を返します。
- PX-S1UD の BS・110度CS と ISDB-C (トランスモジュレーション方式の CATV)。機器と firmware が対応していないため、選局は `EINVAL` を返します。
- MSi2500 の VHF と CATV 帯の地デジ、BS・110度CS、ISDB-C。helper の復調が対応していないため、選局は `EINVAL` を返します。
- CECH-ZD1J の BS・110度CS と CATV 帯。BS・110度CS は機器が受信できず、CATV 帯は実機で確かめていないため、選局は `EINVAL` を返します。
- 32 ビットのプロセスからの受信の ioctl。カーネルモジュールと同じく `compat_ioctl` を提供していません。
- PIX-DT295 W など、別の USB ID の機器。

確認済みの範囲と未確認の条件は、各機種の設計文書を参照してください。
