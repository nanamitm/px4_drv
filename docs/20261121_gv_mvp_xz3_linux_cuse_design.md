# I-O DATA GV-MVP/XZ3 の Linux 対応の設計 (px4-cused)

この文書は、I-O DATA GV-MVP/XZ3 (USB ID `04bb:0530`) を Linux で使うための構成案です。  
PIX-DT295 ([20261120_dt295_linux_cuse_design.md](20261120_dt295_linux_cuse_design.md)) と Sony torne CECH-ZD1J ([20261121_torne_linux_cuse_design.md](20261121_torne_linux_cuse_design.md)) と同じく、ユーザー空間のデーモン `px4-cused` が libusb で機器を動かし、CUSE でデバイスファイルを公開する方式を前提にします。  
維持する契約、WinUSB 版と共有するコード、Linux で測り直す値、試験の範囲、利用者の判断を仰ぐ事項を整理します。  
Linux の動作はすべて設計上の想定で、Linux の実機では確かめていません。  
WinUSB 版での測定値は、出典の文書を併記して引用します。

この文書の「検証環境」は、ほかの 2 機種の設計文書と同じ Raspberry Pi 4 Model B (64 ビット版の Raspberry Pi OS Lite) を指します。

## 目的と範囲

recpt1、BonDriverProxy_Linux、Mirakurun などが、既存の機種と同じく受信のデバイスファイルを開き、PTX の ioctl で地デジ・BS・110度CS を選局できるようにします。  
内蔵カードリーダーは、px4-drv-pcsc (pcscd の IFD ハンドラー) から既存の `include/px4_card_ioctl.h` の契約で使えるようにします。

WinUSB 版の GV-MVP/XZ3 対応は実験的な位置付けです ([gv-mvp-xz3.md](../winusb/docs/gv-mvp-xz3.md))。  
Windows で確かめた範囲は、x64 と x86 での 3 波の選局と受信、110度CS の約2時間57分の受信、内蔵カードリーダーで復号しながらの 110度CS の約3時間25分の受信、PC の USB ポートへ直接つないだ状態での受信中の USB の抜き差しです。  
地デジ・BS の長時間受信、長時間録画、x86 版のカードは Windows でも未確認です。  
Windows でも未確認の条件は、Linux の結果と分けて記録します。

アンテナ給電は WinUSB 版も対応していないため、対象外です (`PTX_ENABLE_LNB_POWER` の 0 以外は `ENOSYS`)。

## ほかの 2 機種との違い

| 項目 | PIX-DT295 | CECH-ZD1J | GV-MVP/XZ3 |
|---|---|---|---|
| 制御転送 | MB86M01 の制御 | vendor request `0xBC` のメモリ要求 | CECH-ZD1J と同じ形式の `0xBC` のメモリ要求 |
| 機器側のプログラム | 起動時に転送 | 使わない | boot0 の RAM の一部を、ホストが中継用のプログラム (`xz3-runtime.bin` など) で一時的に置き換え、閉じるときに元へ戻す |
| 復調器 | TC90522 系 | DiB8096 | TC90522 系 (I²C の `0x18` が地デジ、`0x19` が衛星と推定) |
| 受信できる放送 | 地デジ・BS・110度CS | 地デジ (UHF) | 地デジ・BS・110度CS (1 受信機を共用) |
| TS の取り出し | Bulk IN の連続転送 | Bulk IN の連続転送 | 機器内の 6 MiB のリングから、制御転送の「操作12」で 384 KiB〜3 MiB ずつ Bulk IN へ送り出させる |
| カード UART | MB86M01 の UART | ブリッジの UART (PIX-DT295 と別のクラス) | PIX-DT295 と同じレジスター配置。電源投入と切断の手順だけが異なる (`xz3::CardUart`) |
| シリアル番号 | あり | なし | なし |

GV-MVP/XZ3 の特徴は、TS の取り出し、カード操作、選局が、同じ制御転送の経路 (EP0) を時分割で使う点です。  
操作12 の実行中は EP0 が応答しないため、カード操作は操作12 の合間にしか行えず、操作12 を止めている間も機器内のリングには TS が溜まり続けます。  
WinUSB 版は、この時分割の調停 (送出量の段階の選択、カード操作を待たせる時間) を Windows の実測値で決めています。

## 構成

`px4-cused` に 3 つ目の機種として `Xz3Device` を追加します。  
`Device` 基底クラス、ホットプラグ、受信のリングと PTX の ioctl (`video_node`)、カードのデバイスファイル (`card_node`)、`/run/px4-cused/readers` の対応表は、CECH-ZD1J の追加で機種共通にしたものをそのまま使います。

| デバイスファイル | 内容 |
|---|---|
| `/dev/xz3videoN` (「決めた事項」の 1) | 受信。PTX の ioctl、`read`、`poll` |
| `/dev/px4card/<ID>` | 内蔵カードリーダー。ID は CECH-ZD1J と同じく USB の接続位置から求める |

### 機種ごとの処理の境界

| 層 | 共有するもの | Linux 版だけのもの |
|---|---|---|
| 中継プログラムの導入と復元、操作の要求と応答 (`Relay`) | 照合する RAM の指紋、フックと本体の書き込み順、ステージングの写し、応答の検査 | libusb の制御転送 (CECH-ZD1J の `TornePort` と同じ `torne::ControlTransport`) |
| 選局 (`xz3::Frontend`) | WinUSB 版の `frontend.cpp` をそのまま使う | なし |
| TS の取り出し (操作12 のループ) | 段階の選択 (`NextStage`)、カード操作を待たせる処理 (`HoldForCard`)、`PumpProgress`、`DeliverRecords`、リングの統計 | libusb の非同期 Bulk IN 2 本 |
| カード | `xz3::CardUart`、`dt295::CardResponse`、FIFO の読み出しの間引き | `card_node` の `CardBackend` |
| C/N | `tc90522::CnrMilliDb` | PTX の `GET_CNR` への変換 (PIX-DT295 と同じ `channel.hpp`) |

## WinUSB 版と共有するコード

WinUSB 版の [`xz3/device.cpp`](../winusb/src/DriverHost_PX4/xz3/device.cpp) は、次の部分が Windows の API と結び付いています。

- `Relay` が `WinUsbControl` を直接持ち、中継プログラムを Windows のリソース (`FindResourceW`) から読む
- `BulkRead` が `OVERLAPPED` と `WinUsb_ReadPipe` を使う
- 受信の worker、停止、抜去の待機が `HANDLE` のイベント (`WaitForSingleObject`) を使う
- `HoldForCard` の待機が `Sleep(1)` を使う

CECH-ZD1J と同じ手順で、次の切り出しを先に行い、Windows の実機で回帰試験を済ませてから Linux 版を作る順序にしました (「決めた事項」の 2)。

1. `Relay` を `xz3/relay.{hpp,cpp}` へ移し、制御転送を `torne::ControlTransport` と `torne::BootstrapWait`、期限の設定を小さなインターフェース経由で受け取る。中継プログラムのバイト列は呼び出し側が渡す (Windows はリソース、Linux は `firmware.S` の `.incbin`)
2. 操作12 のループを `xz3/pump.{hpp,cpp}` へ移し、Bulk IN の発行と回収、停止と抜去の待機、短い待機をインターフェースで受け取る
3. カード操作の調停 (`Card`、`card_waiters_`、`card_until_`、`card_last_`) と `IsCardDataReady` の間引きを、`Device` から共有のクラスへ移す

切り出しは動作を変えない変更として行い、Windows の回帰試験 (「試験」を参照) で切り出し前と同じ結果になることを確かめます。  
既存の `xz3_card_test` と `xz3_stream_test` は、切り出した後のクラスを対象に Linux の `make check` でも動かします。

### ライセンスと中継プログラム

中継プログラムは、リポジトリの [`winusb/experimental/xz3-firmware/`](../winusb/experimental/xz3-firmware/README.md) の `relay.c` と `ts_probe.c` (GPL-2.0-only) から FR30 の GCC で生成したもので、生成物の `winusb/pkg/DriverHost_PX4/firmware/xz3-*.bin` もリポジトリにあります。  
メーカーのファームウェアは使わないため、Linux 版も PIX-DT295 と同じく実行ファイルへ埋め込み、別のファイルを配置する手順は不要です。  
Linux のビルドに FR30 のツールチェーンは不要で、生成物の更新は WinUSB 版と同じ手順のままです。

## GV-MVP/XZ3 固有の扱い

### 中継プログラムの導入と復元

WinUSB 版は、開いた後の最初の選局で boot0 の RAM の指紋を照合し、ブートストラップで機器を初期化してから共通版 (`xz3-runtime.bin`) を導入します。  
受信を閉じるときは、フックと本体を元の内容へ戻して読み戻しで照合します。  
TS の DMA の停止に失敗した場合はフックを残し、以後の受信を拒否して USB の挿し直しを求めます (`restore_failed_`)。

Linux 版も同じ手順を使います。  
デーモンの強制終了 (SIGKILL) では復元が行われないため、次にデーモンが起動したときは指紋の照合に失敗し、USB の挿し直しまで受信できない想定です。  
CECH-ZD1J の `Bootstrap` のように起動済みの状態から再び初期化する手順は WinUSB 版にもないため、この制約は `cuse/README.md` に記載し、照合の失敗時のログで挿し直しを促します。  
強制終了の後に挿し直しなしで回復させる手順は、WinUSB 版でも確かめていない変更になるため、この対応の対象外です。

### TS の取り出し

操作12 は、機器内のリング (6 MiB) から指定した量 (3 MiB、1.5 MiB、768 KiB、384 KiB の 4 段階) を Bulk IN (`0x81`) へ送り出させる要求です。  
送出量は 512 バイトの倍数で短いパケットで終わらないため、Bulk IN の 1 回の読み出しは操作12 の送出量と同じ大きさにし、2 本を先に発行します。  
192 バイトのレコードの先頭 4 バイトを外して 188 バイトの TS にします。

libusb では、非同期の Bulk IN 2 本を `PumpSize(stage)` の大きさで発行します。  
1 本が最大 3 MiB になるため、usbfs の転送の上限 (`/sys/module/usbcore/parameters/usbfs_memory_mb`、既定 16 MiB) に収まることを確かめます。  
2 本の合計は最大 6 MiB で、既定の上限に収まる見込みです。

### Linux で測り直す値

次の定数は Windows の実測から決めたもので、理由のコメントに Windows の待機の粒度 (約 16 ms) や制御転送の時間が含まれています。

| 定数 | WinUSB 版の値 | 根拠 |
|---|---|---|
| `PumpTarget` | 250 ms | 操作12 の時間の目標。110度CS で送出が追いつく段階を選ぶ |
| `PumpCardTarget` | 125 ms | カード操作の直後の目標 |
| `CardPollInterval` | 20 ms | 空の受信 FIFO の読み出しを間引く |
| `CardIdleGap` | 50 ms | `CardPollInterval` と Windows の待機の粒度の和 |
| `CardHoldMax` | 150 ms | 操作12 を遅らせる上限 |
| `CnInterval` | 8 回 | C/N を読む操作12 の間隔 |

検証環境の制御転送の往復は、CECH-ZD1J の測定で平均 0.13〜0.21 ms でした (Windows は約 3 ms)。  
ただし、boot0 のメモリ要求の 1 語に約 3 ms かかるという WinUSB 版の記録が、機器側の処理時間か Windows 側の待ち時間かは分かっていません。  
Linux 版は、まず WinUSB 版の値をそのまま使い、操作12 の時間、リングの最大の未処理量、カードの応答時間を測ってから、変える必要があるかを判断します。  
`CardIdleGap` のように Windows の粒度を含む値は、Linux で短くできる可能性がありますが、実測の前には変えません。

### USB の接続経路

WinUSB 版では、USB 2.0 ハブを経由すると操作12 の 1 回に約 140 ms かかって流量に追いつかず、受信できませんでした ([gv-mvp-xz3.md](../winusb/docs/gv-mvp-xz3.md))。  
Raspberry Pi 4 の USB ポートは、本体内の VL805 (USB 3.0 コントローラー) の内蔵ハブを経由します。  
PIX-DT295 と CECH-ZD1J は検証環境で問題なく受信できていますが、GV-MVP/XZ3 は制御転送の往復の時間が流量に直接効くため、最初の実機試験で操作12 の時間とリングの未処理量を確かめます。  
追いつかない場合は、検証環境では受信できない構成として記録し、別の環境 (x86_64 の PC) での確認へ切り替えます。

### 衛星の TS の指定

WinUSB 版の `xz3::Frontend` は、衛星の選局で絶対値の TSID だけを受け付けます。  
recpt1 などの Linux のアプリケーションは、BS のチャンネルを相対 TS 番号 (slot 0〜7) で指定することが多く、PIX-DT295 の Linux 版は共有コードが TMCC から TSID を求めています (`channel.hpp` の規則は `px4_chrdev_set_stream_id_s()` と同じ)。  
GV-MVP/XZ3 でも相対 TS 番号を受け付けるため、選局後に衛星側の復調器から TMCC の TSID の一覧を読み、相対番号を TSID へ変換してから TSID を設定する処理を追加します。  
読み出すレジスターは Linux の TC90522 ドライバーと PIX-DT295 の共有コードを参照して決め、WinUSB 版の選局の手順は変えません。

### カード

カード UART のレジスター配置は PIX-DT295 と同じで、TDA8034AT の CMDVCCN の駆動だけが異なるため、WinUSB 版の `xz3::CardUart` をそのまま使います。  
カード操作は受信の有無に関係なく使え、受信中は操作12 の合間に行います。  
WinUSB 版の受信中の 1 回のコマンドの時間は最大約 0.5〜0.9 秒で、110度CS では受信開始の約 10 秒間に TS の欠落を確認しています ([gv-mvp-xz3.md](../winusb/docs/gv-mvp-xz3.md))。  
Linux 版でも同じ注意 (110度CS の録画には外付けのカードリーダーを検討する) を `cuse/README.md` に記載します。

カードのデバイスファイルを閉じたときは、WinUSB 版と同じくカードへの給電を止めます (`PowerDown`)。  
中継プログラムを復元する条件は、WinUSB 版と同じく受信の `Close` が担当します。  
カードだけが開いている間は中継プログラムを導入せず、1 バイトごとの直接のメモリ要求でカード UART を操作します。

## デバイスファイルの契約

受信は PIX-DT295 と同じ PTX の ioctl の契約です。  
`PTX_SET_CHANNEL` は地デジ (ISDB-T) と衛星 (ISDB-S) の両方を受け付け、`PTX_GET_CNR` は TC90522 の CNDAT を PIX-DT295 と同じ規則で返します。  
受信機は 1 つで地デジと衛星を共用するため、開けるのは同時に 1 つの利用者だけです。

## 機器の着脱とデーモンの寿命

USB の抜去時の扱いは CECH-ZD1J と同じで、開いている利用者がいる間はデバイスファイルを残し、受信は `EIO`、カードは `ENODEV` を返します。  
抜去後は機器の電源が切れるため、RAM の復元は不要です。  
`systemctl restart` は受信を閉じてから終了するため復元が行われますが、受信中の `restart` で停止の応答 (操作13) を待つ時間が systemd の停止の待ち時間に収まるかを確かめます。

## 試験

### 自動試験

- `make -C cuse check` に、`xz3_card_test` と `xz3_stream_test` 相当の試験 (FIFO の読み出しの単位、CRC の分割、部分送出、同期バイトの破損) と、相対 TS 番号の変換の試験を追加する
- `Relay` の切り出しには、模擬の `ControlTransport` で導入・復元の要求列と、指紋の不一致での中止を確かめる試験を追加する

### 実機試験 (検証環境)

CECH-ZD1J と同じ項目を、3 波で行います。

1. 接続、開く処理、指紋の照合、中継プログラムの導入と復元 (閉じた後の RAM の照合)
2. 地デジ・BS・110度CS の各チャンネルを 30 秒受信し、操作12 の時間、段階、リングの最大の未処理量を記録する
3. 地デジのスキャン、無信号からの復帰、BS の相対 TS 番号での選局
4. 受信なしと受信中の APDU (受信中の応答時間の上限を記録する)
5. デーモンの再起動、受信中の SIGKILL (挿し直しが必要になることの確認)、受信中と待機中の USB の抜き差し
6. 地デジ・BS・110度CS の長時間受信と、カード通信を続けながらの長時間受信

### Windows 版の回帰試験

切り出しの後、Windows の PC へ GV-MVP/XZ3 をつなぎ、[gv-mvp-xz3.md](../winusb/docs/gv-mvp-xz3.md) の検証範囲 (3 波の選局、地デジ 13・15ch の `channel_switch_stress`、受信中の `card_reader_reliability`、受信中の USB の抜き差し) を同じ条件で繰り返し、切り出し前と比べます。

## 実装

### 共有コードの切り出し

WinUSB 版の `xz3/device.cpp` から、次のクラスを [`xz3/session.{hpp,cpp}`](../winusb/src/DriverHost_PX4/xz3/session.hpp) へ移しました。  
処理の順序、期限、定数、ログの文言は切り出し前と同じです。

| クラス | 内容 | OS ごとに渡すもの |
|---|---|---|
| `Relay` | 中継プログラムの導入と復元、操作の要求と応答 | `RelayPort` (制御転送、期限、待機)、中継プログラムのバイト列 (`Images`) |
| `Session` | 初期化と選局、操作11〜13 による TS の取り出し (worker)、カード操作の調停、リングの統計 | `SessionPort` (`RelayPort` に、抜去の待機、約 1 ms の待機、Bulk IN の読み出しの生成を加えたもの)、`StreamSink` (TS の受け渡し先) |
| `CardReader` | カードの開閉、ATR と T=1 の受信途中の状態、空の受信 FIFO の読み出しの間引き | 機器の利用可否 (`available_`) |

WinUSB 版の `Device::Receiver` は、受信の状態 (開いているか、選局済みか、失敗したか) と `StreamBuffer` への書き込みだけを持ち、機器の操作を `Session` へ任せます。  
`Device` のカードの各メソッドは `CardReader` へ委ねます。

Linux 版のために、共有コードへ次を加えました。  
WinUSB 版はどれも使わないか既定値で呼ぶため、WinUSB 版の動作は変わりません。

- `Frontend::TuneRelative`: 相対 TS 番号から TMCC の TSID を求めて選ぶ (「衛星の TS の指定」)。`Session::Tune` の `relative` が true のときだけ使う
- `Session::ReadCndat`: PTX の `GET_CNR` に渡す CNDAT の生の値。受信中は TS の転送の応答から更新した値を返す
- `StreamSink::Failed`: worker が転送の異常で終わったことを利用者の `read` へ伝える (既定は何もしない)
- `CardReader::Close(false)`、`Discard`、`PowerDown`: Linux 版の `CardNode` が電源の切断と状態の破棄を別々に呼ぶため

### Linux 版

`cuse/src/xz3_device.{hpp,cpp}` の `Xz3Device` が、`Device` の受信とカードの各メソッドを `Session` と `CardReader` へつなぎます。  
制御転送と期限は CECH-ZD1J の `TornePort` をそのまま使い、Bulk IN は libusb の非同期転送を 1 本ずつ `BulkReader` として作ります。  
WinUSB 版の `WinUsb_FlushPipe` に当たる処理は、libusb がホストにデータを溜めないため何もしません。  
`libusb_clear_halt` は機器のエンドポイントの状態も変え、WinUSB 版にない操作になるため使いません。

受信を閉じるときとデーモンの終了の手順 (`OnShutdown`) で RAM を戻すため、`systemctl restart` の後も挿し直しなしで開ける想定です。  
USB の接続位置からカードリーダーの ID を求める処理は、CECH-ZD1J と共用するため `cuse/src/device.cpp` の `PortCardId` へ移しました (CECH-ZD1J の ID は変わりません)。  
中継プログラムは `src/firmware.S` で実行ファイルへ埋め込みます。  
px4-drv-pcsc の `Info.plist` には `0x04bb`・`0x0530`・`I-O DATA GV-MVP/XZ3 Smart Card Reader` を加えました。

### ビルドと自動試験

- WinUSB 版: `DriverHost_PX4` を x64 と x86 でビルドし、XZ3 のファイルに警告はありませんでした
- Linux 版: WSL の GCC で共有コード (`xz3/session.cpp`、`frontend.cpp`、`card_uart.cpp`) を `-Wall` でビルドし、警告はありませんでした。`xz3_card_test` と `xz3_stream_test` は合格しました
- `cuse/src/xz3_device.cpp` は libusb と libfuse3 のヘッダーが必要なため、検証環境でビルドします

### Windows 版の回帰試験の結果

Windows 11 x64 の PC へ GV-MVP/XZ3 を直接つなぎ、IPC 名を分離した試験用ビルド (`PX4_ISOLATED_TEST_HOST`) の x64 版で、GV-MVP/XZ3 だけを定義した設定から実行しました。  
運用中の DriverHost_PX4 は機器を開いて保持するため、試験の間は利用者が止めました。

| 試験 | 結果 |
|---|---|
| `channel_switch_stress` 地デジ 13ch・15ch を 10 秒ずつ 6 周期 | 688,128 パケット、同期・TEI・CC の異常 0、選局の失敗 0、空受信 0 |
| 同 BS01/TS0・BS15/TS0 を 4 周期 | 544,768 パケット、異常 0 |
| 同 110度CS ND02・ND06 を 4 周期 | 1,355,776 パケット、異常 0 |
| 地デジ 13ch の受信中に `card_reader_reliability` (200 ms 間隔、60 秒) | APDU 63 回すべて成功、1 回の最大 868 ms。TS は 860,160 パケット、異常 0 |
| 110度CS ND06 の受信中に同上 | APDU 95 回すべて成功、最大 508 ms。TS は 2,597,888 パケット、異常 0 |
| 受信なしで同上 (20 秒) | APDU 32 回すべて成功、最大 456 ms |
| 受信を閉じるたびの RAM の復元 | 毎回 `boot RAM restored` |
| 地デジ 13ch の受信中に USB を抜き、約 10 秒後に同じポートへ挿し直す | 抜去までの 351,232 パケットは異常 0。読み出しの失敗で受信を止め (`TS reader failed ret=-5`)、挿し直した機器を約 11 秒後に登録した。`channel_switch_stress` は周期の終わりにだけ `GetSignalLevel()` を呼ぶため、BonDriver はその時点でつなぎ直して選局し直し、成功した (`receiver-reconnect-after-removal.md` の契約どおり) |
| 挿し直した後の地デジ 13ch・15ch を 2 周期 | 229,376 パケット、異常 0 |

機器内のリングの最大の未処理量は 317,760 バイト (リングの約 5%) で、`ring high` は記録されませんでした。  
受信中の 1 回のコマンドの最大は、[gv-mvp-xz3.md](../winusb/docs/gv-mvp-xz3.md) の記録 (約 0.5〜0.9 秒) の範囲です。

カードを挿し直す前は、切り出しの前後のどちらのビルドでもカードの接続が期限切れ (`SCARD_E_TIMEOUT`) で失敗しました。  
挿し直す前の失敗は切り出し前のビルドでも同じだったため、カードの接触の状態によるものと判断しました。  
挿し直した直後の最初の試験では 27 回のうち 1 回が期限切れになり、ホストがカードをリセットして回復しました。  
この 1 回は切り出し前のビルドで同じ条件を再現していないため、原因は確定していません。以降の試験では起きていません。

## Linux 版の実機確認 (途中)

検証環境 (PIX-DT295 を外し、CECH-ZD1J と GV-MVP/XZ3 を本体の USB ポートへ直接接続) で、`px4-cused` の `make check` と px4-drv-pcsc の `make check` が成功し、GV-MVP/XZ3 は `/dev/xz3video0` と `/dev/px4card/<ID>` として認識されました。  
制御転送の往復は平均 159 µs (最大 3.8 ms) で、選局は約 2.1〜2.9 秒、BS の相対 TS 番号 (`BS15_0`) の選局も成功しました。  
停止の手順 (操作13) と RAM の復元は、どの受信でも成功しました (`stop_ok=1`、`boot RAM restored`)。

TS の取り出しには、次の 2 つの問題が残っています。

1. 接続後の最初の受信で、操作11 が成功した後も DMA の書き込み位置 (`PRODUCER`) が 0 のまま進まず、操作12 は毎回約 1.49 秒の待ちの後に送出量 0 を返しました。受信を閉じて開き直すと書き込み位置は進みました。再現の条件は確かめていません。
2. 書き込み位置が進む受信でも、地デジ 13ch の 20 秒で TS が 773,560 パケット (流量から見込まれる量の約 3.4 倍) 届き、CC の飛びが 444,013 件、同期バイトの壊れたレコードの破棄が 102 回記録されました。最初の操作12 から、リングの最大の未処理量がほぼリング全体 (`peak_bytes=6291396`) になり、読む位置 (`cursor=393216`) が書き込み位置 (`producer=323351`) より先にありました。

2 は、中継プログラムの `stream_ring` が `producer < cursor` を周回と見なし、未書き込みの領域を送ったものと見ています。  
最初は、検証環境がリングを読み切るため読む位置が書き込み位置のすぐ後ろを追うことが原因と推定し、操作12 の前に「要求量 + 128 KiB」がリングに溜まるまでホストが待つ処理を試しました。  
待つ処理を入れても、地デジ 13ch の 20 秒で 341,867 パケット (見込みの約 1.5 倍) と CC の飛び 196,835 件、BS15_0 で 253,464 パケットと 142,682 件になり、リングの最大の未処理量は最初からほぼ満杯のままでした。  
WinUSB 版でも最大の未処理量は約 41,000〜317,000 バイトで、読む位置は書き込み位置の近くを追っていたため、この推定は否定され、待つ処理は取り除きました。  
続けて、次の 2 つを切り分けました。

- CECH-ZD1J を外し、GV-MVP/XZ3 だけを接続しても、地デジ 13ch の 20 秒で 627,581〜929,368 パケット、CC の飛びが 41〜61 万件となり、変わりませんでした。USB の給電は原因ではないと判断しました。
- 制御転送のたびに 3 ms の待ちを入れた診断用のビルド (WinUSB 版の制御転送の間隔に近づけたもの) では、選局はロックしましたが、DMA の書き込み位置が 960 バイトで止まり、地デジ・BS とも TS が 1 パケットも届きませんでした。選局の I²C の速さが原因という推定も否定されました。

Raspberry Pi 4 の USB ポートは、すべて本体内の USB 2.0 ハブ (`lsusb -t` の `Class=Hub, 480M`) を経由します。  
GV-MVP/XZ3 を別のポート (同じ内部のハブの 3 番目のポート) へ挿し直しても、接続後の最初の地デジの受信では DMA が始まらず、続く BS15_0 の 20 秒は 607,449 パケット、CC の飛び 334,056 件で、変わりませんでした。  
接続後の最初の受信で DMA が始まらない症状は、挿し直した 3 回のうち 2 回で起きています。  
[gv-mvp-xz3.md](../winusb/docs/gv-mvp-xz3.md) には、WinUSB 版でも USB 2.0 ハブを経由すると受信できず、同じような症状が起きた記録があるため、内部のハブが原因と推定しました。  
この推定を確かめるため、x86_64 の PC (Proxmox VE 9.2 のホスト、Debian 13、カーネル 6.17.2) で、GV-MVP/XZ3 をルートハブのポートへハブを経由せずに接続し、同じビルドを手動で起動して試験しました。  
地デジ 13ch の 20 秒で 641,758 パケットと 800,494 パケット (CC の飛び 349,683 件と 435,724 件)、BS15_0 で 482,395 パケット (同 220,859 件) となり、`ring high` が 128 回、同期の壊れたレコードの破棄が 270 回記録されました。  
この環境では、接続後の最初の受信でも DMA は動作しました。  
ハブを経由しなくても同じ症状になったため、内部のハブが原因という推定は否定され、原因は Linux 版の実装 (libusb での転送の扱いなど) にあると判断しました。  
DMA の書き込み位置がレコードの境界 (192 バイト) に揃わない値 (例: 149,665) になる点と、最初の操作12 で書き込み位置を超えて送出している点は、引き続き調べます。

## 決めた事項

| 事項 | 選択肢 | 設計者の推奨 | 決定 |
|---|---|---|---|
| 1. 受信のデバイスファイルの名前 | `xz3videoN` / `gvmvpxz3videoN` | `xz3videoN` (PIX-DT295 の `dt295videoN` と同じく型番の短い形) | `xz3videoN` (2026-11-21) |
| 2. 共有コードの切り出しの順序 | 切り出しを先に行い Windows の回帰試験を済ませてから Linux 版を作る / Linux 版を先に動かし、後で切り出す | 切り出しを先に行う (CECH-ZD1J と同じ順序) | 切り出しを先に行う (2026-11-21) |

実機試験には、GV-MVP/XZ3 を検証環境と Windows の PC へつなぎ替えて使う必要があります。  
切り出しの回帰試験のため、最初は Windows の PC へつなぎます。

## 未確認の条件

- Linux 版で TS が壊れる原因 (Raspberry Pi 4 と、ハブを経由しない x86_64 の PC の両方で再現する。「Linux 版の実機確認 (途中)」を参照)
- boot0 のメモリ要求の約 3 ms が機器側の時間か
- usbfs での 3 MiB の Bulk IN の挙動
- デーモンの強制終了の後の回復 (挿し直しが必要になる想定)
- 地デジ・BS の長時間受信、長時間録画、x86 版のカード (Windows でも未確認)
