# torne用I²C HAL

`torne_hal.c/.h` はCECH-ZD1JのMB86H47向けに、共通の `i2c_engine.c` を接続する実験用C実装です。本書はWindows上の診断ツールから実行したHALの検証を扱います。別のPAF2診断では、同じHALをソース生成FRイメージへ接続し、torne上でのID読み取りも確認しています。FR版の時計・ABI・ロード/復元は [torne-fr-i2c.md](torne-fr-i2c.md) にあります。既存DriverHostやPAF1からの呼び出しは未接続です。

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

実機ではTXのみのenumeration、TX+RXの識別、100組の値一致、終了時のバスidleとcore1再開、終了後の従来メールボックス経路での識別に成功しています。新HALの実行場所はWindowsであり、新しいFRイメージの実行成功を示す結果ではありません。

## 新HALでの選局・TS受信

```powershell
.\build\probe\torne_hal_probe.exe --receive '<exact torne interface path>' '<TS output prefix>'
python tests/test_capture.py -v
python tools/check_capture.py '<prefix>_gr17_1.ts' --expected-tsid 31760 --discarded-bytes 155
```

`--receive` は識別の後、既存の `NativeFrontend` の全I²C転送を新HALへ接続します。上位層の8bitアドレスはHAL入口で7bitへ変換します。旧core1のCaptrim/TMCCジョブは使わず、上位制御が持つ通常探索を実行します。初期化120秒、各選局60秒などの有限期限を設け、各操作内では期限を更新しません。

試験はGR13→GR17→GR13、測定時間は10秒→60秒→10秒です。受信のDMA、リング、USB送出には既存のソース生成core0と `StreamEngine` を使い、I²C用core1は停止したままにします。測定後もcore1ハートビートが変化していないことを確認してから、終了処理で再開します。3回ともTSを保存し、先頭12,288 bytesの192-byte生データも `.ts.prefix.raw` に保存します。最後に出力をHigh-Zへ戻して復調器をsleepし、TS側のcore0・入力を停止します。

2026-11-05のCECH-ZD1Jで確認した結果は次のとおりです。

| 区間 | 測定時間 | パケット数 | 開始時の同期合わせ | 同期後の破棄 | TEI / CC / 形式エラー | PAT CRC正常数 | TSID |
|---|---:|---:|---:|---:|---|---:|---:|
| GR13 | 10秒 | 114,815 | 97 bytes | 0 | 0 / 0 / 0 | 100 | 31761 |
| GR17 | 60秒 | 688,863 | 155 bytes | 0 | 0 / 0 / 0 | 600 | 31760 |
| GR13再選局 | 10秒 | 114,815 | 184 bytes | 0 | 0 / 0 / 0 | 100 | 31761 |

全区間でロックし、CNRは約24.7～25.0dB、初回TSは各4msでした。復調器制御はHAL経由1,764要求で、core1停止中のハートビートは測定前後で一致しました。終了後はSTOP/idle、core1再開、従来メールボックス経路での識別も確認しています。アンテナ系統や内蔵カードの配置は調査対象外で、スクランブル解除の確認も含みません。

先頭が192-byte境界に揃わない場合があることは、保存済み上流 `../../tools/torne_codegen/reference/stream.go.txt` の `framer.feed` に記載されています。今回の先頭生データでも、最初の3パケットで獲得した境界までのオフセットが破棄総数と一致しています。`check_capture.py` は保存TSの同期・TEI・CC・PAT CRCとTSIDを検査し、ログの破棄総数から初期同期分を引いて途中の破棄がないことを確認します。

この結果は同期後の安定区間の成功であり、開始時を含む無破棄の保証ではありません。診断ツールは全区間の破棄0という厳密判定を保持しているため、この試験の `strict_zero_discard=0`、終了コードは1です。I/O成功と後片付け成功は別に記録します。開始時以外のTEI・CC・形式異常が出た場合は、その区間で受信試験を中断します。各チャンネルは別の測定区間なので、選局遷移中の無欠落を示す試験でもありません。

保存データ検査は通常 `stable_interval_ok` を終了判定に使い、`--strict-zero-discard` を指定すると開始時も含めて破棄0を要求します。`tests/test_capture.py` は既知のPAT/CRCを含むfixtureと、同期・TEI・CC・CRC破損、TSID違い、末尾切断、破棄数不一致の拒否を確認します。

NACK等の実機異常注入、物理波形、長時間転送、DT295固有部は未確認です。別の [PAF2診断](torne-fr-i2c.md) ではソース生成FRイメージへHALとホスト供給時計を接続し、メールボックス経由のID読み取りまで確認しています。新FR版経由の選局・TS受信は次の段階です。
