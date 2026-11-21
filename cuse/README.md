# px4-cused (PIX-DT295、Sony torne CECH-ZD1J、I-O DATA GV-MVP/XZ3 の Linux 版)

PIXELA PIX-DT295 (USB ID `06b8:103b`)、Sony torne CECH-ZD1J (USB ID `054c:04b2`)、I-O DATA GV-MVP/XZ3 (USB ID `04bb:0530`) を Linux で使うためのユーザー空間のデーモンです。  
libusb で機器を動かし、CUSE (Character device in Userspace) で次のデバイスファイルを作ります。

| デバイスファイル | 機種 | 利用者 | 契約 |
|---|---|---|---|
| `/dev/dt295videoN` | PIX-DT295 (地デジ・BS・110度CS) | recpt1、BonDriverProxy_Linux、Mirakurun など | カーネルモジュールの `ptx_chrdev` と同じ PTX の ioctl、`read`、`poll` |
| `/dev/tornevideoN` | CECH-ZD1J (地デジ UHF 13〜62ch) | 同上 | 同上 |
| `/dev/xz3videoN` | GV-MVP/XZ3 (地デジ・BS・110度CS) | 同上 | 同上 |
| `/dev/px4card/<ID>` | 各機種の内蔵カードリーダー | px4-drv-pcsc (pcscd の IFD ハンドラー) | `include/px4_card_ioctl.h` |

設計の理由、契約の詳細、実機での測定は次の文書にまとめています。

- PIX-DT295: [docs/20261120_dt295_linux_cuse_design.md](../docs/20261120_dt295_linux_cuse_design.md)
- CECH-ZD1J と、デーモンを機種共通にした構成: [docs/20261121_torne_linux_cuse_design.md](../docs/20261121_torne_linux_cuse_design.md)
- GV-MVP/XZ3: [docs/20261121_gv_mvp_xz3_linux_cuse_design.md](../docs/20261121_gv_mvp_xz3_linux_cuse_design.md)

制御転送、PIX-DT295 の SERVICE のループ、torne の初期化と選局、GV-MVP/XZ3 の中継プログラムと TS の取り出し、カードの操作は、WinUSB 版 (`winusb/src/DriverHost_PX4/dt295/`、`torne/`、`xz3/`) と同じコードを使います。

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

PIX-DT295 のファームウェアと GV-MVP/XZ3 の中継プログラムは実行ファイルへ埋め込み、torne はメーカーのファームウェアを使わないため、別のファイルを配置する手順はありません。

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

- アンテナ給電 (`PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS`)。どの機種も対応していないためです。PIX-DT295 と GV-MVP/XZ3 で BS・110度CS を受信するには外部からの給電が必要です。
- CECH-ZD1J の BS・110度CS と CATV 帯。BS・110度CS は機器が受信できず、CATV 帯は実機で確かめていないため、選局は `EINVAL` を返します。
- 32 ビットのプロセスからの受信の ioctl。カーネルモジュールと同じく `compat_ioctl` を提供していません。
- PIX-DT295 W など、別の USB ID の機器。

確認済みの範囲と未確認の条件は、各機種の設計文書を参照してください。
