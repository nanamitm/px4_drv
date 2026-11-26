# Sony torne CECH-ZD1J の Linux 対応の設計 (ユーザー空間デーモンと CUSE)

この文書は、Sony torne CECH-ZD1J (USB ID `054c:04b2`) を Linux で使うための構成案です。  
PIX-DT295 の Linux 対応 ([20261120_dt295_linux_cuse_design.md](20261120_dt295_linux_cuse_design.md)) と同じく、libusb で機器を動かし CUSE でデバイスファイルを公開する方式を前提に、維持する契約、WinUSB 版と共有するコード、試験の範囲、決定した事項を整理します。  
本実装は [`cuse/`](../cuse/README.md) の `px4-cused` にあり、Linux の実機で確かめた範囲は「Linux 版の実機確認」に記載します。  
それ以外の節の Linux の動作は、設計上の想定です。  
WinUSB 版での測定値は、出典の文書を併記して引用します。

この文書の「検証環境」は、PIX-DT295 の設計文書と同じく、Raspberry Pi 4 Model B に 64 ビット版の Raspberry Pi OS Lite を入れ、CECH-ZD1J を本体の USB ポートへ直接接続した構成を指します。  
WinUSB 版の回帰試験には、Windows 11 x64 の PC へ同じ CECH-ZD1J をつなぎ替えて使います。

## 目的と範囲

recpt1、BonDriverProxy_Linux、Mirakurun などが、既存の機種と同じく `/dev/*video*` を開き、PTX の ioctl で選局できるようにします。  
内蔵カードリーダーは、px4-drv-pcsc (pcscd の IFD ハンドラー) から既存の `include/px4_card_ioctl.h` の契約で使えるようにします。

CECH-ZD1J の復調器 DiB8096 は地上デジタル (ISDB-T) 用で、WinUSB 版も地デジの UHF だけを扱います。  
このため、BS・110度CS、アンテナ給電、ISDB-T の CATV 帯は対象外です (CATV 帯は「選局の範囲」を参照)。

WinUSB 版の CECH-ZD1J 対応は実験的な位置付けで、README には地デジの約2時間21分の受信と、受信中の USB の抜き差しからの復帰が確認済みと記載されています。  
TVTest での映像の復号、長時間の録画、カード通信を続けながらの長時間受信、複数台の同時利用は Windows でも未確認です ([torne-long-run.md](../winusb/docs/torne-long-run.md)、[torne-integration-notes.md](../winusb/docs/torne-integration-notes.md))。  
Windows でも未確認の条件は、Linux の短時間の結果だけで「確認済み」と記録すると両版の確認範囲の区別が失われるため、Linux 版の記録でも Windows の確認範囲と分けて書きます。

## カーネルモジュールを採用しなかった理由

WinUSB 版の CECH-ZD1J の制御は、PIX-DT295 と同じく、すべてユーザー空間の `DriverHost_PX4.exe` で動いています。  
カーネルの機能を必要とする処理はなく、次の部品をカーネルへ移すと C への書き直しが必要になります。

| 部品 | 規模 | 内容 |
|---|---|---|
| `torne/mb86h47_bootstrap.cpp`、`mb86h47_programs.hpp` | 285 行と 271 行 | ブリッジ MB86H47A の起動、core0・core1 の FR プログラムの配置と生存確認 |
| `torne/mb86h47_i2c.cpp` | 441 行 | core1 経由の I²C (メールボックス、Bulk のコマンド経路、ジョブ) |
| `torne/dib8096_native_generated.inc` ほか | 約 5,000 行 | coax の Go 実装から生成した DiB8000・DiB0090 の制御 |

`dib8096_native_generated.inc` は Go の型情報付き AST から生成した C++ で、カーネルの C へ移すと生成器と比較試験 (`tests/check_torne_native.py`) も作り直す必要があります。  
ユーザー空間ならこれらをそのまま使え、新たに書くのは転送層、受信ループ、デバイスファイルの公開に限られます。  
カーネルモジュールの px4_drv は `054c:04b2` を USB ID の一覧 (`driver/px4_usb.c`) に持たないため、デーモンが機器を開いても既存の機種と衝突しません。  
ほかのカーネルドライバ (DVB など) がこの機器へ割り当てられないかは、検証環境で `lsusb -t` により確かめます。

## PIX-DT295 との違い

同じ libusb と CUSE の方式でも、受信と制御の形は PIX-DT295 と大きく異なります。

| 項目 | PIX-DT295 | CECH-ZD1J |
|---|---|---|
| USB ID | `06b8:103b` | `054c:04b2` |
| 受信方式 | 地デジ・BS・110度CS | 地デジ (UHF) のみ |
| 制御 | `dt295_io` のコールバックで制御転送 (`SourceFirmware`) | vendor request `0xBC` のメモリーアクセス (`MemoryAccess`、最大 64 バイト、big endian) |
| 起動 | 組み込みファームウェアの書き込み (約 0.37 秒) | FR プログラムの配置と I²C の初期化。WinUSB 版の開く処理は約 3.4 秒 (`BulkI2c=true`、制御転送の平均約 3.0 ms の環境) |
| 受信中の制御 | 待機なしの SERVICE のループが制御転送を続ける | 制御転送なし。core0 が TS を SDRAM のリング (6 MiB) から EP `0x81` へ DMA で送る |
| TS の形式 | 上流の `stream_input.c`・`ts_framer.c` が 188 バイトへ組み立てる | 192 バイト (到着時刻 4 バイト + TS 188 バイト) を `StreamFramer` が 188 バイトへ変換 |
| I²C の経路 | 制御転送 | core1 経由。メールボックス (制御転送) か、EP `0x02` (Bulk OUT) と `0x83` (Interrupt IN) のコマンド経路 |
| カード UART | `0xfea0xx` のレジスター。検出は bit 10 が 1 で挿入 | 同じ番地のレジスター。検出は bit 10 が 0 で挿入 (極性が逆) |
| カード UART の経路 | 受信中は PIF1 の Bulk、停止中はブート ROM への直接アクセス | 常に `0xBC` のメモリーアクセス。1 バイトごとに制御転送 |
| USB のシリアル番号 | あり | なし (`iSerialNumber` は 0、[cech-zd1j-validation.md](../winusb/docs/cech-zd1j-validation.md)) |
| 閉じたときの扱い | 起動前の状態へ復元 (`SourceFirmware::Close`) | 復調器を sleep し、ブリッジは起動したまま |

受信中に SERVICE のループがないため、PIX-DT295 の Linux 版で 1 コアの 20〜46% を占めた CPU 時間は、CECH-ZD1J では Bulk IN の完了処理だけになると見込んでいます。  
一方、受信中のカード操作は制御ロックを数百 ms 保持することが WinUSB 版のコメントに記録されており、C/N の読み出しとカード操作の待ち合わせは PIX-DT295 と別の形で考える必要があります (「カード UART」を参照)。

## 構成

デーモンは、接続された CECH-ZD1J ごとに次のものを持ちます。

```
CECH-ZD1J ──USB── libusb の転送層 ── torne の共有セッション (WinUSB 版と共有)
                     │                   Bootstrap / MailboxI2c / NativeFrontend / StreamEngine
                     │                          │
                     │                 制御ロック (1台につき1つ)
                     │              ┌───────────┴────────────┐
             Bulk IN 0x81 (非同期)   CUSE: 受信のデバイスファイル   CUSE: /dev/px4card/<ID>
             192→188 の変換          (PTX の ioctl、read、poll)    (PX4_CARD_* の ioctl)
                     │                     │                          │
                     └──── リング ─────────┘                   px4-drv-pcsc (pcscd)
                                      recpt1 / Mirakurun
```

制御ロックは WinUSB 版の `ExperimentalSession::control_mutex` と同じ範囲を覆います。  
TS の読み出しの待機と、利用者の `read` への応答の間はこのロックを持たず、受信中のロックの利用者は C/N の読み出し、ロックの確認、カードの操作、ストリームの開始と停止に限られます。

### デーモンを PIX-DT295 と共有するか (案 A に決定)

`cuse/` の `px4-dt295d` は、次の点で PIX-DT295 専用の作りになっています。

| 箇所 | PIX-DT295 専用の部分 | 機種に依存しない部分 |
|---|---|---|
| `src/main.cpp` の `Manager` | ホットプラグの登録が `06b8:103b` の 1 組だけ。`Device` の型が固定 | 作業の直列化、空いている最小番号の割り当て、カードの作成の保留、対応表の書き出し、終了の手順 |
| `src/device.cpp` (952 行) | 初期化と復元、SERVICE のループ、`SourceFirmware` の選局、C/N、カードの経路 | リング、`read`・`poll`・`O_NONBLOCK`、PTX の ioctl の受け渡しと `fuse_reply_ioctl_retry`、切断後の `-EIO`、カードの ioctl の受け渡し |
| `src/cuse_session.cpp`、`log.cpp` | なし | CUSE のセッションとシグナルによる起床、ログ |
| `dist/px4-dt295d.service` | 名前と説明 | 権限の絞り込み (`CapabilityBoundingSet=` など) |
| `dist/90-px4-dt295.rules` | `dt295video*` | `px4card/*` |
| `/run/px4-dt295/readers` | パスの名前 | 「バス番号 デバイス番号 パス」の形式 |

次の 3 案を比べました。

| 案 | 内容 | 利点 | 欠点 |
|---|---|---|---|
| A. 1 つのデーモンへ一般化し、中立な名前へ変える | `Manager` を複数の VID/PID へ広げ、`device.cpp` を CUSE の受け渡し (機種共通) と機種ごとの処理に分ける。名前は例えば `px4-cused` とし、ユニット、`/run` の下の対応表も改める | ユニット、udev 規則、対応表、権限の設定が 1 組で済む。両機種を同時につないだときに `/dev/cuse` の利用者が 1 つ | PIX-DT295 の導入済みの環境で、ユニット名と対応表のパスが変わる。IFD ハンドラーは移行期間に新旧の対応表を読む必要がある。PIX-DT295 の回帰試験が必要 |
| B. 1 つのデーモンへ一般化し、名前は `px4-dt295d` のまま | A と同じ分割で、名前と `/run/px4-dt295/readers` を維持する | 導入済みの環境への影響がない | CECH-ZD1J の利用者が `px4-dt295d` を有効にする必要があり、名前が実態と合わない |
| C. CECH-ZD1J 専用のデーモン (`px4-torned` など) を別に作る | `cuse_session`、リング、ioctl の受け渡しを `cuse/src` の共有ファイルとして両方のデーモンでリンクする | PIX-DT295 のデーモンと実行ファイルを分けられ、片方の不具合が他方の受信を止めない | ユニット、udev 規則、対応表が 2 組になり、IFD ハンドラーは対応表を 2 つ読む。`Manager` の終了・保留の手順も 2 か所で維持する |

どの案でも、CUSE の受け渡し (リング、`read`、`poll`、ioctl の再要求、切断後の応答) は 1 か所に置き、機種ごとの処理だけを分けます。  
`device.cpp` には PIX-DT295 の実機で見つかった修正 (カードの作成の保留、使われていないセッションの即時停止、`PTX_GET_CNR` の応答の大きさ) が含まれ、複製すると片方だけが修正される状態を招くためです。

設計者の推奨は案 A です。  
CECH-ZD1J と PIX-DT295 は同じ利用者が同時に使う構成が考えられ、WinUSB 版でも 1 つの `DriverHost_PX4.exe` が両方を管理しています ([torne-long-run.md](../winusb/docs/torne-long-run.md) の検証環境)。  
PIX-DT295 の Linux 版は PR #73 で `develop` へ入ったばかりで、名前を変える費用は今が最も小さいと考えます。  
案 A を選んだ場合、IFD ハンドラーは新しい対応表を優先して読み、旧パスは 1 つの版の間だけ読む移行措置を置きます。  
利用者の判断により、案 A を採用しました (2026-11-21)。

### 機種ごとの処理の境界

案 A・B の場合、CUSE の受け渡しを持つクラスと機種ごとの処理の間の境界は次の操作に限ります。

| 操作 | PIX-DT295 | CECH-ZD1J |
|---|---|---|
| 機器の確認と ID | 組み込みファームウェアの検証、シリアル番号からのカード ID | エンドポイントと alternate の確認、USB の接続位置からのカード ID |
| 開く・閉じる | 初期化と復元 | 共有セッションの取得、復調器の初期化と sleep |
| 選局 | `SourceFirmware::Tune`、ロック後 350 ms の待機 | `NativeFrontend` の選局 (0x11) とロック待ち |
| 受信の開始と停止 | SERVICE のループと Bulk IN 8 KiB × 64 本 | 出力の切り替え (0x12)、`StreamEngine`、Bulk IN の 192→188 変換 |
| C/N とロック | TC90532XBG の CNDAT | DiB8000 の dB を CNDAT へ換算 |
| カード | `dt295::CardSession` | torne のカード操作 (下記) |

受信のリングへ入れる前の TS は 188 バイト単位に揃っている契約とし、192 バイトの外枠は CECH-ZD1J の側で外します。

## WinUSB 版と共有するコード

`winusb/src/DriverHost_PX4/torne/` の部品は、Windows への依存で次の 3 つに分かれます。

| 区分 | ファイル | 扱い |
|---|---|---|
| そのまま使える | `mb86h47_protocol.hpp`、`mb86h47_bootstrap.*`、`mb86h47_programs.hpp`、`mb86h47_i2c.*`、`mb86h47_stream.*`、`mb86h47_card.hpp`、`dib8096_native*`、`dib8096_registers.hpp`、`stream_framer.hpp`、`ts_quality.hpp`、`tune_result.hpp`、`reception_policy.hpp`、`../dt295/card_response.hpp` | Linux 版のビルドから相対パスで参照する |
| Windows の型が混ざる | `frontend_native_client.*` (`HANDLE` の取り消しと停止のイベント)、`frontend_wait.hpp` (高精度の waitable timer) | 取り消し・停止・待機を小さな抽象へ置き換え、Windows 版は現在の実装をその抽象の実装として残す |
| OS ごとに持つ | `winusb_control.*` (`OVERLAPPED` の制御転送とパイプ転送)、`experimental_device.cpp` の `ReadLoop` (`WinUsb_ReadPipe`) | Linux 版は libusb で書く |

残る問題は `experimental_device.cpp` (759 行) です。  
このファイルの `ExperimentalSession` と `Receiver` には、実機の測定から決めた次の処理が含まれ、いずれも Windows の型と混ざっています。

- 制御転送の失敗後に、受信機を開くときだけブリッジを bootstrap から作り直す `Recover` (カードが開いたままだとセッションが作り直されなかった不具合の修正)
- 期限をロックの取得後から数える `CommandWithin` (カード操作がロックを数百 ms 保持し、期限が尽きてセッションを無効にした不具合の修正)
- 選局コマンドを 1 回だけ発行し、Status を最大 100 ms 間隔で確かめる初回ロック待ち (`WaitInitialLock`)、10 秒の操作全体の期限
- 受信の停止で `StreamEngine::Stop` の成否を確かめ、停止状態が不明なときにバスを無効にする手順
- カードの操作 (`OpenCard` から `WriteCardData`) と、`CardResponse` による応答の完成判定

PIX-DT295 で `ControlChannel`、`CardSession`、`TsDelivery` を切り出したのと同じ理由で、これらを Windows に依存しないクラスへ移し、Windows 版と Linux 版の双方から使う形を提案します。

| 共有するもの (仮称) | 内容 | 現在の位置 |
|---|---|---|
| `torne::Session` | 制御ロック、`healthy`、`InitializeBridge`、`InitializeFrontend`、`Recover`、`Invalidate`、`Command`・`CommandWithin`・`ReadStatus` | `ExperimentalSession` |
| `torne::Tuner` (関数群) | 選局コマンド、確定した検索失敗の判定、初回ロック待ち、sleep | `Receiver::TuneFrequency`、`SleepFrontend` |
| `torne::StreamControl` | 出力の切り替え (0x12)、`Prepare`・`Start`・`Stop` の順序と停止の成否 | `ReadLoop` の前後 |
| `torne::TsDelivery` | `StreamFramer`、`TsQuality`、出力先への書き込み、最初の TS の検出、停止時の集計の記録 | `ReadLoop` の内側 |
| `torne::CardSession` | 検出 (bit 10 が 0 で挿入)、リセット、速度、FIFO の読み出しと `CardResponse`、書き込み、電源断 | `ExperimentalDevice::*Card` |
| 状態の解釈 | 24 バイトの Status (ロック、C/N の double、信号強度) | `Receiver::CheckLock`、`ReadStat` |

OS の差は、制御転送 (`ControlTransport`)、待機 (`BootstrapWait`)、パイプ転送 (Bulk のコマンド経路)、取り消しと停止の確認の 4 つの小さな抽象に閉じ込めます。  
Windows の取り消しは `HANDLE` のイベント、Linux の取り消しは PIX-DT295 と同じ `RemovalSignal` と条件変数で実装します。  
`frontend_wait.hpp` の高精度 timer は Windows の tick 単位の遅れを避けるためのもので、Linux では `clock_nanosleep` に相当する待機を、期限と取り消しを確かめながら使います。

ファイルは PIX-DT295 と同じく `winusb/src/DriverHost_PX4/torne/` に残し、Linux 版のビルドから参照します。  
この切り出しは Windows 版の動作に触れるため、Windows 版の回帰試験 (「試験」を参照) を同じ変更の完了条件に含めます。  
利用者の判断により、切り出しを先に行い、Windows 版の回帰試験を済ませてから Linux 版のデーモンを作る順序を採用しました (2026-11-21)。  
この順序なら、同じ処理が 2 か所に存在する期間がなく、Linux 版はすでに Windows の実機で確かめた共有クラスの上に作れます。

### 切り出しの実装

上の表の共有クラスは、次のファイルへ切り出しました。

| ファイル | 内容 |
|---|---|
| `torne/frontend_signal.hpp` | 抜去と受信機の停止要求の確認、フロントエンドの待機の抽象 (`FrontendSignal`) |
| `torne/session.hpp`、`session.cpp` | `SessionPort` (OS ごとの転送と待機)、`Session`、`TuneFrequency`、`SleepFrontend`、Status の解釈、`CardSession`、`TsDelivery` |
| `torne/frontend_signal_win32.hpp`、`winusb_session_port.hpp` | WinUSB 版の `FrontendSignal` と `SessionPort` (従来の `WaitFrontendDelay` と `WinUsbControl` をそのまま使う) |
| `tests/torne_session_test.cpp` | 模擬の `SessionPort` による共有クラスの試験。Windows と Linux の双方でビルドする |

`NativeFrontendClient` は `HANDLE` の代わりに `FrontendSignal` を受け取り、`SetStop(HANDLE)` は `SetStoppable(bool)` になりました。  
受信機の停止要求のイベントは、共有セッションの作成時に渡す必要があるため、受信機から `ExperimentalDevice` へ移しました。  
`experimental_device.cpp` に残る処理は、WinUSB の受信機の登録、`OVERLAPPED` の Bulk IN の読み出しと自動復帰のループ、イベントの待ち合わせです。  
どのビルドでも定義されていない `PX4_TORNE_EXPERIMENTAL` の分岐 (旧診断ヘルパーの経路) は、ビルドの対象外の `frontend_client.cpp` を前提とするため、切り出した処理へは移しませんでした。

GCC (検証環境の arm64、GCC 14.2) で共有コードをビルドした際、Windows の MSVC だけで受理されていた次の 2 点が見つかり、両方のコンパイラーで同じ意味になる形へ直しました。

- `dib8096_native_generated.inc` の名前空間スコープの変数の初期化に `[&]()` のラムダがあり、GCC は既定のキャプチャを持つ非局所のラムダを拒否する。初期化式はローカル変数を参照しないため `[]()` と同じ意味で、生成器 (`tools/torne_frontend/port_native.go.txt`) の `globalInit` で `[]()` を出力するよう変え、生成済みのファイルの該当 14 行へ同じ置き換えを適用した
- `std::tuple<Slice, Error>` を `{{}, error}` で返す箇所を、GCC が曖昧と判定する。`Slice<std::uint8_t>()` を明示した

生成器の変更後の再生成で同じファイルになることは、「Go への依存」に記録した Go 1.26.0 の環境で確かめました。

切り出しの後に、次の自動試験を実行しました。

| 試験 | 結果 |
|---|---|
| `build.ps1` (x86 と x64 のビルド、配布物の作成と検査) | 成功 |
| `torne_foundation_test`、`torne_bootstrap_test` (1,210 か所の故障注入)、`torne_native_frontend_test`、`torne_session_test` (x86 と x64) | すべて成功 |
| 検証環境 (arm64、GCC 14.2、`-Wall -Wextra`) で共有コードと `torne_session_test` をビルドして実行 | 成功。警告は DiB8096 の生成コードと実行時の補助 (`dib8096_native_runtime.hpp`) の既存の記述だけ |

切り出しの後に、Windows 11 x64 の PC へ CECH-ZD1J 1 台をつなぎ、x64 版で回帰試験を行いました。  
既存のアプリケーションとの接続を避けるため、DriverHost_PX4、BonDriver、WinSCard は名前付きパイプの名前を分けた試験用ビルド (`PX4_ISOLATED_TEST_HOST`) を使い、DriverHost_PX4.ini と BonDriver_Torne.ini は配布既定値 (`BulkI2c=true`、`AutoRecovery=false`、`InitialLockRetry=false`) のまま DebugLog だけを有効にしました。  
アンテナは地デジ、カードは B-CAS です。  
比較の基準は、[torne-integration-notes.md](../winusb/docs/torne-integration-notes.md) の `BulkI2c=true` を配布既定値にしたときの記録です。

| 試験 | 切り出し後の結果 | 切り出し前の記録 |
|---|---|---|
| `channel_switch_stress` 13ch と 17ch を交互に 20 回、各 3 秒 | 選局の失敗 0、空受信 0、675,840 パケット、同期・TEI・CC の異常 0 件。切り替えは 1.18〜1.81 秒 (平均 1.27 秒)、開いた後の最初の選局は 2.89 秒 | 選局の失敗 0、675,840 パケット。切り替えは約 1.2 秒 |
| 無信号の 62ch と 13ch を交互に 6 回 | 62ch は 3 回とも 0.58〜1.77 秒で検索失敗を確定し、続く 13ch は 101,376 パケット、異常 0 件 | 0.56〜1.7 秒で検索失敗を確定し、続く 13ch を受信 |
| `card_stream_stress` 13ch (基準 20 秒、並行 60 秒) | APDU 213 回すべて成功、918,528 パケット、異常 0 件 | APDU 211 回成功、919,552 パケット |
| 13ch の 30 分の連続受信 | 20,665,344 パケット、空受信 0、異常 0 件 | 20,665,344 パケット、空受信 0 |
| 受信中に USB を抜き、約 3.6 秒後に挿し直す | USB エラー 31 で受信が停止し、それまでの 1,623,040 パケットは異常 0 件。同じ DriverHost で開き直して 6 回選局し 202,752 パケット、続く APDU 71 回すべて成功 | USB エラー 31 で停止。同じホストで 6 回選局し 202,752 パケット、APDU 31 回成功 |

各受信区間の終了時の DriverHost の記録は、抜去した区間を除いて `stop_ok=1`、TEI、CC の異常、形式不正がすべて 0 件でした。  
x86 版の実機試験と、BonDriver の自動復帰 (`AutoRecovery=true`) は今回の確認の対象外です。

### Go への依存

`winusb/tools/torne_frontend/` (DiB8096 の制御) と `winusb/tools/torne_codegen/` (MB86H47 の FR30 プログラム) の Go のソースは、生成済みの C++ (`dib8096_native_generated.inc`、`mb86h47_programs.hpp`) の生成元です。  
通常のビルドは生成済みの C++ だけを使うため、Windows 版と Linux 版のどちらのビルドと実行にも Go は不要です。

Go のソースは、次の 2 つの理由で残します。

- GPL の対応ソース (改変に適した形) は Go のソースと変換器で、生成済みの C++ は機械的な出力にあたる。`torne_frontend` の Go のコードは coax からの変更 (`LOCAL-CHANGES.md`) を含むため、上流の coax への参照だけでは代わりにならない
- coax の修正を取り込むとき、生成済みの C++ を手で直すと Go 版との比較の根拠を失う。手で C へ移植した torne_drv (mihalinn/torne_drv) では、Bulk のコマンド経路などの上流の改善が取り込まれていない

一方、生成済みの C++ と Go 版の動作の比較 (`check_torne_native.py`) は、試験のたびに Go の模擬バスで正解の記録 (I²C の要求列、待機、結果) を作るため、Go のない環境では実行できません。  
復調器の処理を変えるたびに Go を必要としないよう、Go が作る正解の記録 (9 条件、合計約 144 KB) を `winusb/tests/data/torne_native/` へ置きました。  
`torne_native_frontend_test --golden <正解の記録> <出力先>` が C++ の I²C の要求列、待機、結果をこの記録と比べ、`build.ps1` は x86 と x64 で毎回この比較を実行します。  
Linux 版のデーモンでは `make -C cuse check` から同じ比較を実行します。  
`check_torne_native.py` は、Go で作り直した記録とリポジトリの正解の記録の一致も確かめ、`--update-golden` で記録を書き戻します。  
Go 1.26.0 の環境で次を確かめました。

| 確認 | 結果 |
|---|---|
| 修正後の生成器 (`port_native.go.txt`) で `dib8096_native_generated.inc` を作り直す | 手で置き換えたファイルと完全に一致 |
| `check_torne_native.py --arch x64` と `--arch x86` (Go の模擬バスとの比較、9 条件) | すべて一致 |
| `torne_native_frontend_test --golden` (Windows の x64 と x86、MSVC) | 9 条件すべてが正解の記録と一致 |
| `torne_native_frontend_test --golden` (検証環境の arm64、GCC 14.2) | 9 条件すべてが正解の記録と一致 |

試験の Windows 専用の部分 (`WaitFrontendDelay` の確認) は `_WIN32` の下に分け、Linux でもビルドできるようにしました。  
Go が必要になるのは、coax の取り込みの更新、生成器の変更、正解の記録の作り直しに限られます。  
通常版で使われていなかった旧診断ヘルパー (`tools/torne_frontend/cmd/torne_frontend`) と、それを使う診断 (`torne_usb_probe` の選局と受信のモード、`tests/torne_reference_client.*`、`tests/torne_receiver_adapter.*`、ビルドの対象外だった `torne/frontend_client.*`) は削除しました。  
比較試験 (`check_torne_native.py`) は `internal/coaxd/demod/dib8096` の試験だけを使うため、ヘルパーを除いても `go vet` と `go test` は成功します。  
元の coax の USB 経路と比べる `build_comparison.py` はヘルパーに依存しないため残しました。

### ライセンス

`mb86h47_*` と `cechzd1j` 由来のファイルは coax の `Apache-2.0 OR GPL-2.0-or-later` から GPL を選び、`dib8096` は Linux media 由来の `GPL-2.0-only` です ([cech-zd1j-design.md](../winusb/docs/cech-zd1j-design.md) の「ライセンスと配布」)。  
デーモンは `GPL-2.0-only` で、PIX-DT295 の共有コードと同じ条件でリンクできます。  
Debian パッケージには、元の著作権表示と、`dib8096_native_generated.inc` の生成器 (`tools/torne_frontend/`、`tools/torne_codegen/`) を含む対応ソースの所在を `copyright` に記載します。  
メーカー由来のファームウェアは使わず、core0・core1 の FR プログラムはソース内の生成済みの配列 (`mb86h47_programs.hpp`) だけを使うため、デーモンへ埋め込むファイルや利用者が配置するファイルはありません。

### カード UART を PIX-DT295 と共有しない理由

PIX-DT295 の `dt295/card_uart.cpp` と CECH-ZD1J の `mb86h47_card.hpp` は、同じ `0xfea0xx` のレジスターを操作し、検出の極性が逆です (`dt295/card_uart.cpp` のコメントにも記載)。  
ただし、PIX-DT295 のカード操作は `ControlChannel` を通り、受信中は PIF1 の Bulk 転送、停止中はブート ROM への直接アクセスへ経路を切り替えます。  
CECH-ZD1J の操作は `MemoryAccess` の 1 経路で、待機中の制御ロックの扱いも異なります。  
1 つのクラスへまとめると PIX-DT295 の Windows 版と Linux 版のカード経路の回帰試験が必要になり、この作業の範囲を超えるため、統合は見送りました。  
両者が同じレジスターを使うことは、将来まとめる場合の手がかりとしてここに残します。

## torne 固有の扱い

### USB の確認

WinUSB 版の `ExperimentalDevice::Init()` と同じく、開いたハンドルの VID/PID、interface 0、alternate 0、`0x81` が Bulk で最大パケット 512 バイト、`0x02` が Bulk、`0x83` が Interrupt であることを確かめ、どれかが違えば開きません。  
`0x84` (Bulk、512 バイト) は WinUSB 版も coax も使っていないため、Linux 版でも使用の対象外です。

### 制御転送 (vendor request `0xBC`)

`MemoryAccess` は 1 回の要求を 64 バイト以下に限り、実際の転送長が要求と一致しないものを `short_transfer` として失敗にします。  
Linux の `ControlTransport` は libusb の同期制御転送で実装し、PIX-DT295 の `UsbTransport` と同じく次の契約を守ります。

- 期限は初期化、選局などの操作全体に 1 回だけ設定し、各転送には残り時間を渡す
- 失敗した要求は再送しない (応答が不明な I²C 要求がメールボックスに残る可能性があり、`Session` がバスを無効にする)
- USB の切断は `RemovalSignal` で待機中の処理へ直ちに伝える

### Bulk のコマンド経路 (`0x02` と `0x83`)

WinUSB 版の配布既定値は `BulkI2c=true` で、2 回目以降の選局が制御転送の経路の 2.92〜2.99 秒から 0.97〜1.07 秒へ短くなりました ([torne-integration-notes.md](../winusb/docs/torne-integration-notes.md))。  
Linux 版もこの経路を既定にし、`0x02` は `libusb_bulk_transfer`、`0x83` は Interrupt のエンドポイントのため `libusb_interrupt_transfer` で扱います。  
起動時に `0x83` に残るデータを回収し、確認用のコマンドの応答を検証する手順は `MailboxI2c::EnableBulk` のものをそのまま使います。  
デーモンには設定ファイルがないため、制御転送の経路 (`BulkI2c=false`) へ戻す手段は診断用のコマンドライン引数だけとする案です。  
`BatchI2cWrites`、`InitialLockRetry`、`AutoRecovery`、`OutputErrorPackets` は WinUSB 版の配布既定値 (いずれも `false`) と同じ動作に固定し、Linux 版の設定は不要と考えます。

### TS の 192 バイトの外枠

EP `0x81` は 192 バイト単位 (先頭 4 バイトが到着時刻、続く 188 バイトが TS) で届き、USB の転送の境界は 192 バイトの境界と一致しません。  
`StreamFramer` は端数を保持し、TEI のない同期バイトが 192 バイト間隔で 3 つ続いたときに境界を確定し、同期済みの TEI は計数して上位へ渡します。  
同期の判定には `driver/ts_sync.h` の `px4_ts_has_plain_sync()` を使うため、Linux 版で条件を複製する必要はありません。  
チャンネルを変えるときは、受信を止めて読み出しを回収した後に `StreamFramer` を作り直し、前のチャンネルの端数を次のチャンネルへ持ち越しません (WinUSB 版の `ReadLoop` と同じ)。

WinUSB 版の `ReadLoop` は 6,144 バイトの読み出しを 1 本ずつ投入しています。  
Linux 版は libusb の非同期転送を複数本投入し、PIX-DT295 と同じく完了ごとにリングへ入れる形を想定します。  
1 本の長さは 192 と 512 の公倍数 (1,536 の倍数) とし、初期値は coax の上限 (64 KiB) 以下で、検証環境の Bulk IN の完了の間隔と CPU 時間を測ってから決めます。  
地デジの約 2.16 MB/秒 ([torne-long-run.md](../winusb/docs/torne-long-run.md)) に対し機器内のリングは 6 MiB で、約 2.9 秒分の停止を吸収できる計算です。

### ストリームの開始と停止

WinUSB 版の順序を維持します。

1. 出力の切り替えのコマンド (0x12、引数 1)
2. `StreamEngine::Prepare()` (TS エンジン、SDRAM のリング、core0、EP1 の DMA)
3. EP `0x81` のリセット (`libusb_clear_halt`) と読み出しの投入
4. `StreamEngine::Start()`
5. 停止は、読み出しの取り消しと回収の後に `StreamEngine::Stop()`、続けて 0x12 (引数 0)

`Stop()` が失敗した場合はセッションを無効にし、次に受信のデバイスファイルを開くと `Recover` で作り直します。  
Bulk IN の取り消しが5秒以内に完了しない場合、受信の再開には USB の挿し直しが必要です。  
未回収の転送とバッファは libusb が使い続ける可能性があるため、完了通知が届くまで解放せずに保持し、遅れて届いた完了通知の中で解放します。  
WinUSB の `WinUsb_ResetPipe` と libusb の `libusb_clear_halt` が機器へ同じ要求を送るかは、検証環境で確かめます。

WinUSB 版は受信の開始後 2 秒以内に最初の TS が届かなければ失敗にしています。  
`PTX_START_STREAMING` はカーネルモジュールでは即座に戻るため、Linux 版も最初の TS を待たずに戻り、2 秒以内に届かない場合はログに記録して `read` を `-EIO` で終える案です。

### カード UART

`CardUart` は `0xfea01c` の bit 10 が 0 のときに挿入と判定します (PIX-DT295 は 1 で挿入)。  
FIFO の読み書きは 1 バイトごとの制御転送です。  
WinUSB 版では、受信と同時の APDU が通常は数十 ms でしたが、初期化や選局と重なると最大 2,582〜3,753 ms になりました ([cech-zd1j-card-design.md](../winusb/docs/cech-zd1j-card-design.md)、[cech-zd1j-native-frontend.md](../winusb/docs/cech-zd1j-native-frontend.md))。  
受信中に制御転送を使う処理はカードと C/N の読み出しだけのため、カード操作が TS の流れを止めることはないと見込みますが、選局の時間とカードの応答時間は互いに延びます。  
この遅れは Windows でも上限が確認されておらず、Linux 版でも選局と APDU を重ねた試験で記録します。

速度は ATR の指定に従い 19200 bps (B-CAS) と 38400 bps (A-CAS) を受け付けます。  
`CardResponse` は受信を 518 バイトに制限し、ATR または T=1 の必要なバイトが揃うまで `PX4_CARD_RX_READY` を偽にします。  
カードの抜去で ATR と T=1 の状態を破棄する契約と、2 バイトに満たない T=1 の応答を `-EPROTO` にする `SmartCard` の修正 (PR #73) は IFD ハンドラー側にあるため、CECH-ZD1J にも同じく適用されます。

## デバイスファイルの契約 (受信)

デバイスファイルの名前は `/dev/tornevideoN` とします (2026-11-21 に利用者が選択)。  
既存の名前は、カーネルモジュールが `px4video`、`pxmlvideo`、`isdb2056video` など、CUSE のデーモンが `dt295video` で、いずれも機種か系統の名前に連番を付けています。  
番号は空いている最小の番号を割り当てます。

| ioctl・操作 | 応答 | 備考 |
|---|---|---|
| `open` | 2つ目の open には `-EALREADY` | `ptx_chrdev_open()` と同じ。開いたときに共有セッションを取得し、復調器を初期化する |
| `PTX_SET_CHANNEL` | UHF 13〜62ch (`freq_no` 63〜112) を選局 | 換算式は `cuse/src/channel.hpp` (`driver/ptx_chrdev.c` と同じ) |
| `PTX_SET_CHANNEL` (CATV、衛星) | `-EINVAL` | 下記の「選局の範囲」 |
| `PTX_SET_SYSTEM_MODE` | `PTX_UNSPECIFIED_SYSTEM` と `PTX_ISDB_T_SYSTEM` だけ受け付ける | |
| `PTX_START_STREAMING`、`PTX_STOP_STREAMING` | 二重の開始・停止は `-EALREADY` | |
| `PTX_GET_CNR` | DiB8000 の dB を CNDAT へ換算した値。応答は `sizeof(int)` | 下記の「C/N」 |
| `PTX_GET_LOCK_STATUS` | Status のロック (受信中は最初の TS の到着も条件) | WinUSB 版の `CheckLock` と同じ |
| `PTX_ENABLE_LNB_POWER` | `arg` が 0 なら成功、0 以外なら `-ENOSYS` | PIX-DT295 と同じ |
| `PTX_GET_TMCC_*`、`PTXT_*` | `-ENOSYS` | 必要な利用者の有無を確認してから追加 |
| `read`・`poll`・`O_NONBLOCK` | PIX-DT295 と同じ | |
| `close` | 受信を止め、復調器を sleep (0x14) する。カードが開いていればブリッジは残す | WinUSB 版の `Receiver::Close` と同じ |
| USB 切断後 | すべての操作に `-EIO` | PIX-DT295 と同じ |

### 選局の範囲

WinUSB 版は 473,143〜767,143 kHz (UHF 13〜62ch) 以外を `-EINVAL` で断っています。  
DiB0090 のチューナー自体は VHF・CATV 帯の周波数を扱える可能性がありますが、CECH-ZD1J の機器での受信は Windows でも確かめておらず、AGC やフィルターの表がこの帯域を含むかも確認していません。  
このため Linux 版も同じ範囲に限り、CATV 帯は確認できた時点で WinUSB 版と同時に広げます。

選局の期限は WinUSB 版と同じく、選局コマンド、ロック待ち、最初の TS の受信を含めて 10 秒です。  
確定した検索失敗は期限を待たずに返り、PIX-DT295 と同じく機器の異常として扱わず次の選局を受け付けます (無信号の errno は PIX-DT295 の `-ENOLINK` にそろえる案)。  
選局が 10 秒の期限を越えた場合、または I²C の失敗でセッションが無効になった場合は、閉じて開き直すまで操作を断ります。

DiB8096 のロック直後に訂正前の TS が出るかは、WinUSB 版の記録にありません。  
`StreamFramer` は TEI のある候補を境界の根拠にせず、WinUSB 版の長時間受信で TEI は 0 件でしたが、Linux 版の最初の受信でロック直後の TEI の有無を記録してから待機の要否を決めます。

### C/N

`PTX_GET_CNR` の利用者 (recpt1 など) は、値を TC90522 の地デジの CNDAT とみなして dB へ換算します。  
DiB8000 は C/N を dB の実数で返すため、デーモンは recpt1 の地デジの換算式の逆を数値的に解き、同じ dB になる CNDAT を返します。  
CXD2856ER を使う PX-MLT 系と XIT-SQR100 も、同じ目的で換算表 (`driver/cxd2856er_cnr.c`) を使っています。  
読み出しは制御ロックを取るため、PIX-DT295 と同じく 1 秒に 1 回までとし、間の要求には前回の値を返します。

## デバイスファイルの契約 (内蔵カードリーダー)

`/dev/px4card/<ID>` の ioctl の応答は PIX-DT295 の CUSE のデーモンと同じです (同時に開けるのは 1 つ、電源投入前の UART の操作は `-EHOSTDOWN`、切断後は `-ENODEV`、`close` で電源を切る)。  
カードだけを開いた場合は、WinUSB 版と同じくブリッジを bootstrap し、復調器、LED、TS は起動しません。

### リーダーの ID

PIX-DT295 の ID は USB のシリアル番号から求め、カーネルモジュールと WinUSB 版と同じ値になります。  
CECH-ZD1J はシリアル番号を持たず、WinUSB 版の ID は Windows のデバイスインスタンスのパス (接続位置を含む) から求めているため、Linux で同じ値を再現できません。  
Linux 版の ID は、sysfs の USB の接続位置 (`1-1.2` などのポートの経路) から FNV-1a で求めた 16 桁の 16 進数とします。

- 同じポートへ挿し直すと同じ ID になり、PIX-DT295 と同じ「古いセッションが終わるまで作成を保留する」手順がそのまま働く
- 別のポートへ挿すと ID が変わり、pcscd のリーダー名も変わる
- バス番号とデバイス番号は挿し直すたびに変わるため、ID には使わない
- Windows と Linux で同じ機器の ID は一致しない (README と pcsc/README.md に記載する)

### IFD ハンドラー

`pcsc/Info.plist` へ `0x054c`/`0x04b2` と `Sony torne CECH-ZD1J Smart Card Reader` を追加します。  
`ResolveCardDevice()` は CUSE のデバイスを対応表で照合するため、デーモンが CECH-ZD1J の行を同じ形式で書けば、コードの変更は対応表のパス (案 A の場合) だけで済みます。

## 機器の着脱とデーモンの寿命

受信とカードは、WinUSB 版と同じく 1 台につき 1 つの共有セッション (bootstrap 済みのブリッジ) を参照で共有します。  
最初の利用者が bootstrap し、最後の利用者が閉じたときにセッションを手放します。  
受信のデバイスファイルを閉じても、カードが開いていればブリッジを止めません。

USB の切断は `RemovalSignal` と `available` を制御ロックの下で無効にし、カードのセッション (ATR と T=1 の状態を含む)、受信中の読み出し、共有セッションをまとめて破棄します。  
開いている利用者がいる間はデバイスファイルを残し、全員が閉じてから消す手順は PIX-DT295 と同じです。

`Bootstrap` は二重 bank の検証と切り替えで、すでに起動したブリッジを再び起動できる作りで、WinUSB 版の初期の診断では同じ個体の起動を 2 回続けて成功させています ([cech-zd1j-validation.md](../winusb/docs/cech-zd1j-validation.md))。  
廃止された source-fr の経路では、受信中にホストを強制終了した後、抜き差しなしの新しいホストで再び受信できました ([torne-source-firmware.md](../winusb/docs/torne-source-firmware.md))。  
現行の経路で、プロセスの強制終了の後に抜き差しなしで受信できるかの記録はなく、デーモンの再起動と異常終了からの復帰は Linux の実機で確かめます。  
bootstrap が起動できない状態と判定した場合 (`not_power_up_state` など) は、明示的に失敗させ、抜き差しを促すログを残します。

## 権限と導入

権限の考え方は PIX-DT295 と同じです (root で動かし、systemd のユニットで特権を外し、`/dev/cuse` と USB 機器だけを開く)。  
udev 規則には、受信のデバイスファイルの名前に対応する `SUBSYSTEM=="cuse"` の行を追加します。  
カードの `px4card/*` の規則は共通です。  
案 C の場合は、ユニットと規則が CECH-ZD1J 用に別に必要です。

## 試験

実機試験は冒頭で定義した検証環境で行い、試験の記録には CPU アーキテクチャ、カーネルと libusb と libfuse3 の版、アンテナの接続、カードの種類 (B-CAS・A-CAS)、試験時間を記載します。  
カード試験の記録は応答の長さと SW だけとし、カードのデータを残しません。

### 自動試験

- `make -C cuse check` に、`torne_foundation_test` (protocol、framer、カード UART、Status、待ち合わせ) と `torne_bootstrap_test` (1,129 か所の故障注入を含む) を Linux でもビルドして加える。現在の試験は `winusb_control.hpp` と `frontend_wait.hpp` を通じて Windows のヘッダーを読むため、試験の Windows 専用部分を分ける
- `check_torne_native.py` が使う Go の模擬バスとの比較を Linux でも実行できるか確かめる (Go が必要なため、`make check` からは外す可能性がある)
- 切り出した `Session`・`CardSession`・`TsDelivery` を模擬の `ControlTransport` で Windows と Linux の双方から試す
- 受信のデバイスファイルの ioctl の戻り値、CATV と衛星の `-EINVAL`、`PTX_GET_CNR` の応答の大きさと CNDAT の換算 (逆算した値を recpt1 の式へ戻して元の dB に近いこと)

### 実機試験 (検証環境)

| 試験 | 記録する値 |
|---|---|
| 接続直後の bootstrap と開く処理 | 所要時間、制御転送の回数と平均・最大、Bulk のコマンド経路の起動の成否 |
| UHF 13〜62ch の走査 | 各チャンネルの選局時間、ロックの有無、検索失敗の確定時間 |
| recpt1 で 30 秒と 10 分 | 同期、TEI、CC の飛び、形式不正、`discarded`、Bulk IN の完了の間隔、`read` の間隔、CPU 時間 |
| 6 時間の受信 (TS は保存せずに計数) | 上記に加え、RSS、CPU 温度、PID ごとの途切れの後の CC の飛び |
| 受信と pcscd の APDU の同時動作 (短時間と 6 時間) | APDU の成功数と失敗数、平均・最大の応答時間、TS の異常 |
| 選局を繰り返しながらの APDU | 選局時間とカードの最大応答時間の延び |
| 受信していない間と受信中の USB の抜き差し | デバイスファイルの削除と再作成、対応表の更新、再接続後の受信 |
| カードの抜き差し (受信なし・受信中) | 抜去で返るエラー、挿入から最初の APDU の成功までの時間 |
| デーモンの再起動と強制終了 (SIGKILL) からの復帰 | 抜き差しなしで再び受信できるか |
| 受信中の SIGTERM | 終了時間、デバイスファイルの削除 |
| PIX-DT295 と同時の接続と受信 (案 A・B の場合は同じデーモン) | 両方の TS の異常、CPU 時間、片方の抜き差しが他方へ影響しないこと |

確認できない条件は、Windows でも未確認の条件 (映像の復号、長時間の録画、複数台) と分けて記録します。  
検証環境には CECH-ZD1J が 1 台しかないため、複数台の同時利用は Linux でも未確認のまま残ります。

### Windows 版の回帰試験 (共有コードの切り出しを行う場合)

`build.ps1` で x86 と x64 をビルドし、`run_torne_tests.py` と `check_torne_native.py --arch x86/x64` の自動試験を実行します。  
実機では、Windows 11 x64 の PC へ CECH-ZD1J をつなぎ替え、[torne-integration-notes.md](../winusb/docs/torne-integration-notes.md) の `BulkI2c=true` の既定値を決めたときの試験 (13ch・17ch の交互の選局 20 回、無信号の 62ch を含む切り替え、受信中のカード APDU、30 分の受信、受信中の USB の抜き差し) を同じ条件で繰り返し、切り出し前の結果と比べます。

## Linux 版の実装

`px4-dt295d` を機種共通のデーモン `px4-cused` へ組み替え、CECH-ZD1J を加えました。

| ファイル | 内容 |
|---|---|
| `cuse/src/video_node.*`、`card_node.*` | 受信とカードのデバイスファイルの機種共通の部分。旧 `device.cpp` のリング、`read`、`poll`、PTX の ioctl とカードの ioctl の受け渡しを、順序と応答を変えずに移した |
| `cuse/src/device.*` | 機器 1 台の共通部分 (2 つのノード、切断、セッションの寿命)。機種ごとの処理は `VideoBackend` と `CardBackend` |
| `cuse/src/dt295_device.*` | PIX-DT295 の固有の処理。旧 `device.cpp` からそのまま移した |
| `cuse/src/torne_device.*`、`torne_port.*` | CECH-ZD1J の固有の処理と、共有の `torne::Session` へ渡す libusb 版の `SessionPort` と `FrontendSignal` |
| `cuse/src/main.cpp` | 機種の表 (VID/PID) からホットプラグを登録する。受信のデバイスファイルの番号は機種ごとに独立 |
| `cuse/dist/px4-cused.service`、`90-px4-cused.rules` | ユニットと udev 規則の名前を変え、`tornevideo*` の規則を加えた。ユニットは `px4-dt295d.service` と同時に動かない |
| `pcsc/` | 対応表の既定を `/run/px4-cused/readers` にし、既定の表を使う場合は旧パス `/run/px4-dt295/readers` も読む。`Info.plist` に `054c:04b2` を加えた |

CECH-ZD1J の処理の要点は次のとおりです。

- 開くときに共有セッションを取得し、`Recover` の後に `InitializeFrontend` を呼ぶ (Bulk のコマンド経路、書き込みの一括化)。閉じるときは復調器を休止させ、カードが開いていればブリッジを残す
- 選局は `TuneFrequency` を 10 秒の期限で呼び、確定した検索失敗と期限内のロックなしを `-ENOLINK` にする。I²C や USB の失敗は、閉じて開き直すまで操作を断る
- 受信は、出力の切り替え、`PrepareStream`、`libusb_clear_halt(0x81)`、Bulk IN 24 KiB (1,536 × 16) × 16 本の投入、`StartStream` の順で始める。2 秒以内に最初の TS が届かなければ `read` を `-EIO` で終える
- C/N は DiB8000 の dB を、recpt1 の式で同じ dB になる CNDAT へ換算する (`cuse/src/channel.hpp` の `DbToCndat`)
- 待機は `RemovalSignal::WaitUntil` で期限の時刻まで待ち、ミリ秒より短い指定も切り上げない

`make -C cuse check` は、PIX-DT295 の既存の試験に加えて、`torne_session_test` と、DiB8096 の生成コードの正解の記録との比較を実行します。

## Linux 版の実機確認

検証環境で、systemd のユニットで動かした `px4-cused` に PIX-DT295 と CECH-ZD1J を同時に接続して確かめました。  
カーネルは 6.18.50、libusb は 1.0.28、libfuse3 は 3.17.2、受信は recpt1 (stz2012 版) です。  
TS の計数は同期、TEI、CC の飛び、同じ CC の内容違い、形式不正で、受信の終了時のデーモンの記録 (リングのあふれ、`stop_ok`) も確かめました。  
カードは B-CAS で、試験は応答の長さと SW だけを記録しました。

| 試験 | 結果 |
|---|---|
| `make -C cuse check`、`make -C pcsc check` (arm64、GCC 14.2) | すべて成功 |
| 接続直後 | 両機種の受信とカードのデバイスファイルを作成し、`/run/px4-cused/readers` に 2 行を記載。CECH-ZD1J はほかのカーネルドライバに割り当てられていなかった (`Driver=usbfs`) |
| 開く処理 | bootstrap は制御転送 1,218 回 (平均 132 µs)、フロントエンドの初期化は制御転送 23 回と Bulk のコマンド経路 166 回 |
| CECH-ZD1J の 13ch を 30 秒 | 340,863 パケット、異常 0 件。recpt1 の C/N の表示 24.65 dB、最初の選局 2.53 秒、受信の開始から最初の TS まで 12 ms、Bulk IN の完了の間隔は最大 11.3 ms |
| CECH-ZD1J の 13ch を 2 時間 (カード通信なし) | 82,663,039 パケット (約 17.3 Mbps)、異常 0 件、リングのあふれ 0、`stop_ok=1`。受信中の同期の喪失はなく、境界合わせで捨てたのは受信開始時の 111 バイトだけ。Bulk IN の完了の間隔は最大 11.9 ms、利用者側の読み出しの間隔は最大 13 ms。デーモンの CPU は 2%、RSS は 31 MB のまま増えず、SoC の温度は 44.8〜47.2 °C |
| CECH-ZD1J の 13ch を 2 時間受信しながら、pcscd 経由で B-CAS の INT を 100 ms ごとに送る | APDU 71,800 回すべて成功 (平均 54.2 ms、最大 125.9 ms)、接続し直し 0。TS は 82,657,919 パケットで、同期の喪失、TEI、リングのあふれは 0、`stop_ok=1`。CC の飛びは 1 件で、約 60 秒途切れていた PID 0x0111 が再び現れたときのもの (下記)。Bulk IN の完了の間隔は最大 13.8 ms、利用者側の読み出しの間隔は最大 14 ms。デーモンの CPU は 9〜11%、RSS は 31 MB のまま増えず |
| UHF 13〜62ch の走査 (recpt1 で各 2 秒) | 13・15・16・17ch がロック (C/N 24.5〜26.1 dB)。無信号の 46 チャンネルは 1.5〜3.9 秒で検索失敗を確定。14ch は 1 回だけ 3.9 秒で検索失敗を確定した (下記) |
| 14ch を続けて 5 回 | 5 回ともロック (選局 2.1〜2.6 秒、C/N 25.2〜26.0 dB) |
| PIX-DT295 の 13ch を 30 秒 | 593,571 パケット、異常 0 件 (組み替えの前と同じ動作) |
| 2 台の同時受信 60 秒 (PIX-DT295 は 15ch、CECH-ZD1J は 13ch) | 1,179,691 パケットと 678,527 パケット、どちらも異常 0 件。CECH-ZD1J の Bulk IN の完了の間隔は最大 23.4 ms |
| pcscd 経由の APDU、受信なし | CECH-ZD1J 50 回すべて成功 (平均 54.9 ms)、PIX-DT295 50 回すべて成功 (平均 70.9 ms) |
| CECH-ZD1J の 13ch を 120 秒受信中に APDU | 1,000 回すべて成功 (平均 54.3 ms、最大 127.6 ms)。TS は 1,376,639 パケット、異常 0 件で、Bulk IN の完了の間隔は最大 11.6 ms |
| PIX-DT295 の 13ch を 60 秒受信中に APDU | 500 回すべて成功 (平均 57.6 ms)。TS は 1,180,911 パケット、異常 0 件、SERVICE の最大 9.05 ms |
| 受信していない間の `systemctl restart` の後 | 抜き差しなしで開き、166,399 パケット、異常 0 件 |
| CECH-ZD1J の受信中に SIGKILL | systemd が 5 秒後に起動し直し、抜き差しなしで CECH-ZD1J は 165,759 パケット、PIX-DT295 は 291,033 パケット、どちらも異常 0 件 |
| CECH-ZD1J の受信中に USB を抜き、約 2.5 秒後に挿し直す | Bulk IN の失敗で受信を止め、それまでの 506,495 パケットは異常 0 件。開かれたままの `/dev/tornevideo0` を残して `/dev/tornevideo1` とカードのデバイスファイル (同じ ID) を作り、対応表を更新した。挿し直した機器の 15 秒の受信は異常 0 件 |
| 受信していない間に抜き、約 3.4 秒後に挿し直す | `/dev/tornevideo0` を作り直して対応表を更新し、15 秒の受信は異常 0 件 |

制御転送の往復は平均 0.13〜0.21 ms で、WinUSB 版の測定環境 (約 3 ms) より短く、選局はおおむね 2.0〜2.7 秒でした。  
受信中に制御転送を使わないため、カードの操作は Bulk IN の完了の間隔に影響しませんでした。

`Bootstrap` は二重 bank の検証と切り替えで起動済みのブリッジを再び起動できるため、デーモンの再起動と、受信中の強制終了の後のどちらも、CECH-ZD1J の抜き差しは不要でした。  
デーモンの強制終了や USB の抜去で受信が途切れたとき、recpt1 は終了コード 0 で終わったため、録画が途中で止まったことは recpt1 の終了コードからは判別できません。

カード通信を伴う 2 時間の受信の CC の飛びは、PID 0x0111 が約 60 秒 (59,969 ms) 送られなかった後に再び現れた最初のパケットで記録されました。  
同じ時刻に同期の喪失、TEI、リングのあふれ、読み出しの遅れがなく、ほかの PID の CC は連続していたため、放送側で PID が一時的に止まったものと判断しました。  
カード通信のない 2 時間の受信より CPU が増えたのは、pcscd と IFD ハンドラーを経由する 1 秒あたり約 10 回の APDU の処理のためです。

走査での 14ch の検索失敗は、上流の探索が無信号として確定したもので、WinUSB 版の既定 (`InitialLockRetry=false`) でも同じく返します。  
1 回だけの観測で、頻度と条件は確かめていません。

## 決定した事項

利用者の判断を仰いだ事項と、その結果は次のとおりです。

| 事項 | 選択肢 | 設計者の推奨 | 決定 |
|---|---|---|---|
| デーモンの構成 | A. 1 つへ一般化し中立な名前へ変更 / B. 1 つへ一般化し `px4-dt295d` のまま / C. CECH-ZD1J 専用のデーモン | A | A (2026-11-21) |
| WinUSB 版との共有コードの切り出しの順序 | 切り出しを先に行い Windows の回帰試験を済ませてから Linux 版を作る / Linux 版を先に動かし、後で切り出す | 切り出しを先に行う | 切り出しを先に行う (2026-11-21) |
| 受信のデバイスファイルの名前 | `tornevideoN` / `cechzd1jvideoN` | `tornevideoN` | `tornevideoN` (2026-11-21) |

デーモンの新しい名前は `px4-cused` とし、ユニットは `px4-cused.service`、対応表は `/run/px4-cused/readers` に決めました。

設計者が決めた次の事項は、異論があれば見直します。

- 選局の範囲は UHF 13〜62ch に限り、CATV 帯は WinUSB 版で確かめるまで対象外
- カードリーダーの ID は USB の接続位置から求め、Windows の ID とは一致しない
- Bulk のコマンド経路を既定にし、ほかの WinUSB 版の設定は配布既定値に固定する
- カード UART のクラスは PIX-DT295 と共有しない (理由は「カード UART を PIX-DT295 と共有しない理由」)
- `PTX_START_STREAMING` は最初の TS を待たずに戻る

## 未確認の条件

「Linux 版の実機確認」で、ほかのカーネルドライバの割り当て、制御転送の往復時間、デーモンの再起動と強制終了の後の bootstrap は確かめました。  
次の条件は未確認です。

- 2 時間を超える受信と、カード通信を続けながらの 2 時間を超える受信
- Bulk IN の転送の長さと本数の見直し (24 KiB × 16 本で、単独では完了の間隔が最大約 11 ms、PIX-DT295 と同時で最大約 23 ms)
- `libusb_clear_halt` と `WinUsb_ResetPipe` が機器へ同じ要求を送るか (受信の開始は成功している)
- DiB8096 のロック直後の訂正前の TS の有無 (選局から最初の TS までに TEI は記録されていない)
- 走査で 1 回起きた 14ch の検索失敗の頻度と条件
- pcscd がカードを使っている最中の USB の抜き差しと、カードの抜き差し
- x86_64 の Linux でのビルドと動作
- 選局とカード操作を重ねたときの、カードの応答時間の上限 (Windows でも未確認)
- 映像の復号、長時間の録画、複数台の同時利用 (Windows でも未確認)
- A-CAS での受信を伴う長時間のカード通信 (Windows でも未確認)
