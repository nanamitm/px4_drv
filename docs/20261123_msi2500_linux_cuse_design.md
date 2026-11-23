# MSi2500 (RSP1 互換機) の Linux 版 (px4-cused) の設計と試験の記録

この文書の「x86_64 の試験機」は、USB ハブを経由せずに機器をつなげる x86_64 の Proxmox VE のホスト (Intel N150、4 コア、Debian 13 系のカーネル 6.17。同じホストで仮想マシンが動いている) を指します。  
「WinUSB 版の検証環境」は、[msi2500-fullseg-design.md](../winusb/docs/msi2500-fullseg-design.md) の検証環境 (Windows 11 x64、Ryzen 9 5900X) を指します。

## 目的と範囲

SDRplay RSP1 の互換機 (Mirics MSi2500 + MSi001、USB ID `1df7:2500`) で、地上デジタル放送のフルセグと J.83 Annex B を Linux で受信します。  
WinUSB 版と同じ helper (`winusb/tools/msi2500_frontend`) が OFDM 復調、誤り訂正、A・B 階層の再多重を行い、ほかの機種と同じ `px4-cused` (CUSE のデーモン) が PTX の ioctl を持つ `/dev/msi2500videoN` を作ります。  
カードリーダーとアンテナ給電はありません。

helper の処理量は Intel N150 でも実時間に間に合うよう、先に誤り訂正の段を分けて速くしました ([msi2500-fullseg-design.md](../winusb/docs/msi2500-fullseg-design.md) の 11.5 節)。  
Raspberry Pi 4 は、OFDM 復調と誤り訂正の処理量が足りないため対象外です。

## 構成

USB は RTL-SDR の Linux 版 ([20261122_rtlsdr_linux_cuse_design.md](20261122_rtlsdr_linux_cuse_design.md) の A) と同じくデーモンが持ち、helper は標準入力で生ブロックを受けて復調だけを行います。  
RTL-SDR で A を選んだ理由 (デーモンが接続位置で機器を選び、ほかの用途の機器を開かないこと、カーネルのドライバーを外して戻す処理をほかの機種と揃えること、helper が USB を開かず systemd の制限を受けないこと) は MSi2500 にもそのまま当てはまります。

| デバイスファイル・プロセス | 内容 |
|---|---|
| `/dev/msi2500videoN` | 受信。PTX の ioctl、`read`、`poll`。J.83B は拡張 ioctl (`PTXT_SET_PARAMS`、`PTXT_TUNE`) |
| helper (`msi2500_frontend serve --stdin`) | 選局のたびに `px4-cused` が子プロセスとして起動し、標準入力で 1024 バイトの生ブロックを受け、標準出力へ TS と状態を返す (WinUSB 版の `serve` と同じ形式) |
| helper (`msi2500_frontend ops`) | 選局のたびに、デーモンが送る制御転送の列を出して終わる |

### レジスターの設定の列

MSi2500 と MSi001 のレジスターの計算は、WinUSB 版の helper の `regs.rs` (libmirisdr の移植、`tests/regs_golden.rs` で libmirisdr の出力と照合済み) にあります。  
C++ のデーモンへ書き写すと、同じ計算が 2 か所に分かれ、片方の修正がもう片方へ反映されない危険があります。  
このため、helper に `ops` を加え、`serve` と同じ引数 (周波数、利得、方式) から、取り込みの前に送る列 (`setup`) と停止要求の後に送る列 (`stop`、ADC の休止) を 1 行 1 回の制御転送として出させ、デーモンはそれを送るだけにしました。  
x86_64 の試験機でビルドした helper の出力は、WinUSB 版の helper の出力と一致しました。

### 共有するコード

| 層 | 共有するもの | Linux 版だけのもの |
|---|---|---|
| helper | `msi2500_frontend` のすべて (OFDM 復調、誤り訂正、再多重、J.83B、レジスターの計算) | `serve --stdin` と `ops` の入口 (Windows でも動く) |
| helper の出力の形式と監視 | RTL-SDR と同じ `rtlsdr/protocol.hpp` と `rtlsdr::Session` (起動し直しの回数と待機、ロックの判定、MER) | なし |
| helper の子プロセス | RTL-SDR の Linux 版の `RtlSdrHelperProcess` (標準入力のキューとあふれの扱い) | なし |
| USB | なし (WinUSB 版は helper が WinUSB で取り込む) | `cuse/src/msi2500_device.cpp` の等時転送と制御転送 |

`rtlsdr::Session` はログの機種名を固定の "RTL-SDR" で書いていたため、機種名を引数で受け取るようにしました (既定値は "RTL-SDR" で、RTL-SDR の動作は変わりません)。  
WinUSB 版の MSi2500 の受信機 (`msi2500/msi2500_device.cpp`) は、`rtlsdr::Session` を切り出す前から同じ処理を自前で持っていて、今回は変えていません。  
WinUSB 版も `rtlsdr::Session` へ移すかは、Windows の実機での回帰試験と合わせて別に判断します。

## MSi2500 固有の扱い

### 機器の選び方

MSi2500 は汎用の SDR で、Linux の本体の `msi2500` ドライバー (V4L2 の SDR、`/dev/swradioN`) や SDRplay の API でも使われ、シリアル番号を返さない個体もあります。  
RTL-SDR と同じく、環境変数 `PX4_CUSED_MSI2500_PORTS` (カンマ区切り) に書いた接続位置 (sysfs と同じ「バス番号-ポートの経路」) の機器だけを開きます。  
接続位置で選ぶ処理は RTL-SDR と同じ関数 (`device.cpp` の `UsbPortListed`) を使います。  
x86_64 の試験機には別の SDRplay の機器 (RSPdx、`1df7:3030`) が仮想マシンに割り当てられていますが、製品 ID が異なるため対象になりません。

### カーネルのドライバー

ほかの機種と同じく、指定した機器を開くときに interface 0 から `msi2500` を外し、デーモンを止めるときと機器を手放すときに戻します。  
外した後は `swradio` のデバイスが消え、戻すと再び作られることを実機で確かめました。

### 取り込み

WinUSB 版の helper (`usb.rs`) と同じ手順で取り込みます。

1. `ops` の `setup` の列を送る (宛先はデバイス、`bmRequestType` 0x40。宛先をエンドポイントにすると WinUSB が一部の書き込みを拒否した)
2. interface 0 を alternate 1 (等時転送 IN 0x81、1 マイクロフレームに 3 × 1024 バイト) へ切り替える
3. helper を起動し、等時転送を投入してから開始要求 (0x43) を送る
4. 停止では、転送を止めて完了を待ち、20 ms あけて停止要求 (0x45) と `stop` の列 (ADC の休止) を送り、alternate 0 へ戻して等時転送の帯域の予約を解放する

転送の停止の前に停止要求を送ると、長めの取り込みの後で次の開始でデータが届かない状態に入ることが WinUSB 版で確認されていて、Linux の本体の `msi2500` ドライバーも同じ順に止めます。

1 転送は 64 パケット (8 ms) で、32 転送 (256 ms 分) を投入します。  
WinUSB 版の helper は 64 転送 (512 ms 分) を投入しますが、usbfs は既定で全機器の転送のバッファーを合計 16 MB に制限するため (`usbfs_memory_mb`)、64 転送 (約 12.6 MB) ではこの機器だけで大半を使います。  
32 転送で、x86_64 の試験機の受信で失敗したパケットと転送の失敗が起きないことを確かめました (「実機確認」)。

完了したパケットは、状態が成功で長さが 1024 の倍数の部分だけを helper へ渡します。  
失敗したパケットと空のパケットは除き、helper の OFDM がブロック番号の欠落として扱います (1 パケットの失敗で転送ごと捨てると欠落が 64 倍に広がるため、WinUSB 版の helper と同じ扱い)。  
データが 3 秒届かない場合と、転送の失敗が 64 回続いた場合は、WinUSB 版の helper の `CaptureLimits` と同じく受信の失敗とし、`rtlsdr::Session` が helper と受信を起動し直します。  
helper の標準入力のキューがあふれた場合も、生ブロックを捨てて続けると欠落を隠すため、RTL-SDR と同じく失敗として起動し直します。

### 選局

地デジは `PTX_SET_CHANNEL` で、RTL-SDR と同じく UHF 13〜52ch (`freq_no` 63〜102、`slot` は 0) を受け付けます。WinUSB 版の 473,143〜707,143 kHz の検査と同じ範囲です。  
J.83B は PX-S1UD と同じく `PTXT_SET_PARAMS` で放送方式 `PTX_J83B_SYSTEM` と中心周波数 (kHz) を受け、WinUSB 版と同じ 90,000〜775,000 kHz を受け付けます。

選局の成功は、最初の TS が届いたことを意味します (カーネルの契約と同じ)。  
最大 15 秒待ち、届かなければ `EAGAIN` を返します。  
WinUSB 版の検証環境では地デジの最初の TS まで約 2 秒で、helper は 10 秒 TS が出なければ復調を作り直すため、弱電界の作り直しを 1 回含められる RTL-SDR と同じ値にしました。

### C/N と利得

`PTX_GET_CNR` は、helper が報告する MER (地デジは B 階層、J.83B は判定帰還中の復調器) を、PX-S1UD と同じく recpt1 などの地デジの換算式で同じ dB になる値で返します。  
利得は MSi001 の合計 (dB) で、WinUSB 版の `GainDb` と `GainDbJ83B` と同じ既定値 (40 と 46) を、環境変数 `PX4_CUSED_MSI2500_GAIN` と `PX4_CUSED_MSI2500_GAIN_J83B` で変えられます。  
B 階層のビタビのスレッド数は `PX4_CUSED_MSI2500_VITERBI_THREADS` (0 は helper の既定) で変えられます。

### helper の監視と自動の起動し直し

`rtlsdr::Session` が行い、条件と回数は RTL-SDR と同じです (helper の予期しない終了、形式の異常、30 秒の無応答、受信の失敗で、同じ周波数のまま起動し直す。連続 8 回まで)。  
helper の起動し直しでは、`ops` の取得、制御転送、alternate の切り替え、等時転送もやり直します。

## 実装

- helper: `serve` を Windows 専用から外し、`--stdin` (標準入力の生ブロック、入力の終わりを停止要求とする) を加えた。`--path` の取り込みは従来どおり Windows だけ。`ops` を加えた
- `cuse/src/msi2500_device.{hpp,cpp}`: `Msi2500Device`。`main.cpp` の機種の表に、カーネルのドライバーを外す指定と `Msi2500PortSelected` を加えた
- `cuse/src/device.{hpp,cpp}`: 接続位置の取得と選択を `UsbPortPath` と `UsbPortListed` に切り出し、RTL-SDR も使う
- `winusb/src/DriverHost_PX4/rtlsdr/session.{hpp,cpp}`: ログの機種名を引数にした
- `cuse/Makefile`: `make -C cuse msi2500` と `install-msi2500` (PX-S1UD の J.83B と同じ helper で、firmware は不要)
- `cuse/dist/90-px4-cused.rules`: `msi2500video*`

### ビルドと自動試験

- x86_64 の試験機 (Debian 13 の GCC 14): `make -C cuse` に新たな警告はなく、`make -C cuse check` はすべて成功した。`make -C cuse msi2500` (Debian の cargo 1.85) も成功した
- WinUSB 版: helper の `cargo test` (録画 IQ を使う試験を含む) と release のビルドが x86・x64 で成功した。`serve --stdin` で録画 IQ を流し、TS が出ることと、入力の終わりで止まることを確かめた。DriverHost_PX4 は `rtlsdr::Session` の変更の後も x64 でビルドできた (`rtlsdr_session_test` は Linux の `make check` で成功)

## 実機確認

x86_64 の試験機のルートハブのポート `3-4` へ RSP1 互換機をつなぎ、`px4-cused.service` と同じ制限 (`DevicePolicy=closed` など) を付けた試験用のデーモンで確かめました。  
アンテナは WinUSB 版の検証環境と同じ地域の地上波とケーブルの配線で、録画は recpt1、TS の検査は同期、TEI、CC の飛びを数える試験用のプログラムで行いました。

| 試験 | 結果 |
|---|---|
| 13ch を 60 秒 | 682,592 パケット、同期・TEI・CC の異常 0。helper の RS 訂正不能 0、再多重の捨てたパケット 0。USB の失敗したパケットと転送の失敗 0。選局 1,827 ms、C/N 表示 27.18 dB。CPU は helper 約 1.24 コア、デーモン約 2% |
| 受信の 20 秒目に helper を SIGKILL | デーモンが起動し直し、1,826 ms で受信が戻った。recpt1 は続き、欠けた区間は CC の飛び 28 回として現れた。同期と TEI の異常 0 |
| J.83B の H008 (138,750 kHz) を 60 秒 | 1,617,667 パケット、同期・TEI・CC の異常 0。RS 訂正不能 0、MER 31.1 dB、選局 270 ms |
| 1 回開いたまま 13・15・17ch を 20 秒ずつ 6 周 | 18 区間で 4,177,455 パケット、同期・TEI・CC の異常 0、別の TSID の PAT 0。TSID は各チャンネルで正しかった。選局 1,885〜2,082 ms |
| 受信中に `systemctl stop` | 178 ms で止まり、helper は残らず、`msi2500` ドライバーが戻って `/dev/swradio0` が作られた |
| 13ch を 2 時間 | 82,580,159 パケット、同期・TEI の異常 0、CC の飛び 544 回。USB の失敗したパケットと転送の失敗 0、デーモンと helper の起動し直し 0。CC の飛びはすべて開始から 69 分目の 1 分間にあり、helper はこの間にサンプルの番号の飛びを 147 回 (失ったサンプル 30,639,276) 検出して補った。RS 訂正不能は B 階層 80,620、A 階層 1,909 |

2 時間の受信で欠けた区間では、番号の違う複数のブロックが同じサンプルのバイト列を持っていて、機器が古いデータを送った可能性があります。  
同じ時刻のカーネルのログに USB のエラーはなく、原因は特定できていません。  
試験機は同じルートハブへ他の仮想マシンの USB 機器がつながり、直前の 15 分平均の負荷は 4.8 でした。WinUSB 版の実機で同じ現象が起きるかも確認していません。

`ptx_switch` (1 回開いたまま選局を繰り返す試験用のプログラム) は各区間の最初のパケットが PAT であることも条件にしていて、RTL-SDR と同じくこの条件だけで全区間が FAIL になりました。  
helper の TS は再多重の出力の任意のパケットから始まり、WinUSB 版も同じです。

## 未確認の条件

- 2 時間の受信で起きた、サンプルの番号の飛びの原因 (機器、USB の帯域の競合、試験機の負荷のどれか)
- 待機中と受信中の USB の抜き差し
- J.83B の長時間の受信と、地デジと J.83B の切り替え
- 変更後の helper の、WinUSB 版の実機での受信 (helper の `serve --path` の経路のコードは引数の解析の整理だけで、取り込みと復調は変えていない)
- x86_64 の試験機以外の Linux と、仮想マシンの中での利用
