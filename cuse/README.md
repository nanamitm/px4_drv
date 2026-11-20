# px4-dt295d (PIX-DT295 の Linux 版)

PIXELA PIX-DT295 (USB ID `06b8:103b`) を Linux で使うためのユーザー空間のデーモンです。  
libusb で機器を動かし、CUSE (Character device in Userspace) で次のデバイスファイルを作ります。

| デバイスファイル | 利用者 | 契約 |
|---|---|---|
| `/dev/dt295videoN` | recpt1、BonDriverProxy_Linux、Mirakurun など | カーネルモジュールの `ptx_chrdev` と同じ PTX の ioctl、`read`、`poll` |
| `/dev/px4card/<ID>` | px4-drv-pcsc (pcscd の IFD ハンドラー) | `include/px4_card_ioctl.h` |

設計の理由、契約の詳細、実機での測定は [docs/20261120_dt295_linux_cuse_design.md](../docs/20261120_dt295_linux_cuse_design.md) にまとめています。  
制御転送、SERVICE のループ、カードの操作は WinUSB 版 (`winusb/src/DriverHost_PX4/dt295/`) と同じコードを使います。

## ビルド

libusb-1.0 と libfuse3 の開発用パッケージが必要です。  
カーネルモジュールと異なり、カーネルのヘッダーと DKMS は不要です。

```bash
sudo apt install build-essential pkg-config libusb-1.0-0-dev libfuse3-dev
make -C cuse
make -C cuse check
```

`make check` は USB と CUSE を使わない自動試験で、WinUSB 版と共有する制御経路、選局の換算、SHA-256 と組み込みファームウェアを確かめます。  
選局と TS 転送に使うファームウェアは実行ファイルへ埋め込むため、別のファイルを配置する手順はありません。

## 導入

```bash
sudo make -C cuse install
sudo udevadm control --reload
sudo systemctl daemon-reload
sudo systemctl enable --now px4-dt295d
```

`/dev/cuse` を開けるプロセスは任意の名前でデバイスファイルを作れるため、デーモンは root で動かし、systemd のユニットで特権 (capability) を外して、開けるデバイスを `/dev/cuse` と USB 機器に限ります。  
作られたデバイスファイルの権限は `dist/90-px4-dt295.rules` で設定し、受信は `video` グループ、カードは `pcscd` グループに許可します。

内蔵カードリーダーを pcscd から使うには、px4-drv-pcsc ([pcsc/README.md](../pcsc/README.md)) も導入します。  
IFD ハンドラーは、デーモンが書く `/run/px4-dt295/readers` で USB 機器とカードのデバイスファイルを対応させるため、pcscd のホットプラグで認識されます。

## 動作

機器を接続すると、受信のデバイスファイルを空いている最小の番号で作り、カードのデバイスファイルを USB のシリアル番号から求めた ID で作ります。  
ID は Windows 版のリーダー名に表示される ID と同じです。  
受信のデバイスファイルを閉じるたびに、機器を起動前の状態へ戻します。

機器を抜いたときは、開いている利用者がいる間はデバイスファイルを残し、受信は `EIO`、カードは `ENODEV` を返します。  
全員が閉じた時点でデバイスファイルを消します。  
同じ機器を挿し直した場合、pcscd が古いカードのデバイスファイルを閉じるまで、新しいカードのデバイスファイルの作成を保留します。

デーモンのログは journald に記録されます (`journalctl -u px4-dt295d`)。  
受信の終了時には、TS の計数 (同期、TEI、CC の飛び) と SERVICE の回数と最大所要時間、その時刻を記録します。

## 対応していないもの

- アンテナ給電 (`PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS`)。PIX-DT295 が対応していないためです。BS・110度CS には外部からの給電が必要です。
- 32 ビットのプロセスからの受信の ioctl。カーネルモジュールと同じく `compat_ioctl` を提供していません。
- PIX-DT295 W など、別の USB ID の機器。

確認済みの範囲と未確認の条件は、設計文書の「本実装の確認」と「未決の事項と未確認の条件」を参照してください。
