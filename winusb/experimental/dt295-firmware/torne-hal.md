# torne用I²C HAL

`torne_hal.c/.h` はCECH-ZD1JのMB86H47向けに、共通の `i2c_engine.c` を接続する実験用C実装です。専用のWindows診断ツールからUSBレジスターアクセスを渡し、実機でDiB8096の識別と反復読み取りを確認しています。既存DriverHostやPAF1メールボックスからの呼び出しは未接続で、FR版の実機実行とCAPSのI²C機能ビット有効化は今後の段階です。

## 根拠とライセンス

転送シーケンスはCopyright 2026 Till0196の[coax core1prog.go](https://github.com/Till0196/coax/blob/471328e03942219544e78004e961d8f69d27f46f/internal/coaxd/usb/bridge/mb86h47/core1prog.go)を参照・翻案しています。元ソースはApache-2.0 OR GPL-2.0-or-later、本実装ではGPL-2.0を選択します。保存済み原典・NOTICE・COPYINGは `../../tools/torne_codegen/` にあります。初期化手順は既存 `../../src/DriverHost_PX4/torne/mb86h47_bootstrap.cpp` の `I2cInit()` と照合しています。不透明なファームウェアバイナリはビルド入力にしません。

| 操作 | torne側の値・条件 |
|---|---|
| 状態 / 制御 / データ | `0xfd8fe0` / `0xfd8fe4` / `0xfd8ff0` |
| バス使用中 | 状態bit 7 |
| バイト完了 / バス異常 | 制御bit 0 / bit 7 |
| アドレス・送信データのNACK | 完了後の状態bit 4 |
| START・送信 / repeated START | 制御 `0x50` / `0x70` |
| 受信途中 / 最終byte | 制御 `0x58` / `0x50` |
| STOP要求 | 制御 `0x40`、その後バス使用中が解除されるまでpoll |

torneモードの制御値は毎回上書きし、前の完了bitやrepeated STARTを次のbyteへ持ち越しません。従来のDT295用の暫定マスク操作は別モードとして保持します。torneで動作してもDT295側のアドレス・クロック・割り込みの正しさは保証しません。

## 呼び出し契約

1. 呼び出し側が対象機器を識別し、クロック、他CPUの停止、排他所有、接続世代を確立します。`exclusive` はその全条件を確認し、各I/O直前に呼ばれます。単なる「USBが開けた」という条件では不足します。全APIは単一所有者で直列に呼び出します。
2. `dtp_torne_init()` はソフトウェア状態だけを初期化します。`bus` の読み書きはCPU物理アドレス・ホスト順の値で行い、成功1/失敗0を返します。USBの転送エンディアン変換、アクセス権限、バリアはアダプター側で実装します。失敗した書き込みの適用有無は不明として扱います。
3. `dtp_torne_prepare()` が既存のI²C初期化手順を実行します。広域クロック設定は含みません。最後に状態のidleを確認します。これは機器を書き換える操作であり、受信中・旧core1実行中には使えません。レジスターの全bitのreadbackや電気的な安定の確認は未実装です。
4. `dtp_torne_start()` は7bitアドレス、TX/RX各最大40bytes、転送全体の `timeout_us` を受け取ります。`now_us` は単調増加する32bitマイクロ秒時計で、転送中も更新される必要があります。1回の期限は1～`0x7fffffff`。poll回数やCPUループ回数を時間と見なしません。I/Oコールバック自体の有限時間化は呼び出し側の責務です。
5. `dtp_torne_poll()` は制御bit 0で進行し、固定期限とSTOP後のidleを確認します。成功時だけ `hal.engine.rx[0..rx_completed)` を採用します。アドレスNACK、データNACK、バス異常、期限切れ、所有権喪失、I/O失敗を区別します。

途中失敗は再利用禁止にします。所有権・I/O失敗後は以後のレジスター操作を止めます。その他の転送エラー時はSTOPを要求しますが、バス回復の成功とは扱いません。`tx_submitted` は投入数、`tx_completed` はACK確認済み数で、未確認byteの適用有無は断定できません。I/O失敗時の受信データ・カウンターは結果として使用できません。自動再試行や `prepare()` による暗黙回復は行いません。再初期化には呼び出し側で機器全体の静止・回復を確認する必要があります。

実機確認にはDiB8096の7bitアドレス `0x09`、レジスター896（TX `03 80`、RX 2bytes）と897を使用しています。2026-11-05のCECH-ZD1Jでvendor ID `0x01b3`、device ID `0x8002` を取得し、100組の反復読み取りも一致しました。既存APIの8bitアドレス `0x12` を新APIへ渡すと別アドレスになるため、変換が必要です。

## ビルドと試験

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic -I . i2c_engine.c torne_hal.c tests/test_torne_hal.c -o test-torne
./test-torne
python3 build_auxiliary.py --with-torne-hal --out build/torne-hal
```

FRビルドの `--with-torne-hal` は未接続のHALをリンクしてコンパイル・サイズを検証するだけです。エントリーは引き続き診断用補助CPUプログラムであり、torne用USB起動イメージではありません。manifestの `deployable=false`、`features=[]` は維持します。

模擬試験では既存と同じ初期化順序・アクセス幅、7bit変換、完了待ち、repeated START、受信ACK/最終NACK、STOP待ち、初期化全I/O位置の失敗、転送I/O失敗、NACK分類、世代喪失、時刻wrapを確認します。模擬試験だけでは物理SCL/SDA、割り込み、実時間、USB転送、CPU間排他の成立は検証できません。

## Windowsからの実機診断

```powershell
MSBuild.exe torne_hal_probe.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
.\build\probe\torne_hal_probe.exe --run '<exact torne interface path>'
```

診断対象は明示した1個体のVID `054c` / PID `04b2` です。共有なしでデバイスを開くため、DriverHostが未使用のtorneを保持している場合も、解放されるまで診断は失敗します。他チューナーを受信中のDriverHostを自動終了する機能はありません。

診断は既存のソース生成ブート経路でアドレス拡張・クロックを設定し、既存core1の停止要求とハートビート停止を確認してから新HALを呼びます。`now_us` はWindowsのsteady clock、MMIOコールバックは既存 `MemoryAccess` へ接続しています。core1制御レジスターは書き込み値の全bitを読み返せないため、未確認bitの一致を停止条件に使いません。

復調器リセットと既存のレジスター1287へのenumeration書き込みを行い、vendor IDを2回、device IDを1回読み、その後両IDを100組確認します。転送全体の500ms期限はUSB往復を含む診断用の値で、製品向けの実測境界ではありません。

終了時はSTOP後のバスidleと既存core1のハートビート再開を確認します。ブート状態は従来nativeドライバーと互換の初期化済み状態に残り、電源投入前像への完全復元ではありません。途中の初期化・停止・再開が失敗した場合は再利用の前にUSB再接続が必要です。専用ツールはその旨を表示し、失敗終了します。

実機ではTXのみのenumeration、TX+RXの識別、100組の値一致、終了時のバスidleとcore1再開、終了後の従来メールボックス経路での識別に成功しています。新HALの実行場所はWindowsであり、FRイメージの実行成功を示す結果ではありません。NACK等の異常注入、物理波形、長時間転送、新HALでの選局・TS受信、DT295固有部は未確認です。次はソース生成FRイメージにHALと機器側時計を接続し、メールボックス経由で同じIDを確認する段階です。
