# ソースから作る補助CPUコードとI²C状態機械

本書は既定のPAF1診断用FRイメージと、DT295向けI²Cの実験用C状態機械を扱います。PAF1からI²C状態機械への接続はなく、通常のBonDriver配布物には入りません。別ビルドのtorne用PAF2では、HALとホスト供給時計を接続したFRイメージの実機ID読み取りを確認しています。その実装・検証範囲は [torne-fr-i2c.md](torne-fr-i2c.md) にあります。

## ソースと構成

| ファイル | 役割 |
|---|---|
| `auxiliary_entry.s` | FR入口、スタック設定、BSS初期化、Cへの接続 |
| `auxiliary.c` / `.h` | 独自PAF1診断メールボックス、heartbeat、PING/CAPS、I²C未準備応答 |
| `i2c_engine.c` / `.h` | 副作用をI/Oコールバックへ分離した、非ブロッキング転送状態機械 |
| `runtime.c` | コンパイラーが必要とする場合のmemcpy/memset。外部ランタイムは不要 |
| `auxiliary.ld` | コード/データ、BSS、スタック、メールボックスの配置制約 |
| `build_auxiliary.py` | 同じCをFR用にコンパイル・アセンブル・リンクしmanifestを生成 |

ビルドに公式/mAgicVISIONのファームウェアバイナリや、展開した参考補助コードは不要です。動作の根拠となる静的解析は [research.md](research.md) にあります。

## 配置とPAF1

補助CPU側の入口は `0x000a0000`、スタック上端は `0x000a37fc`、メールボックスは `0x000a3900`。主CPUから見える共有RAMはそれぞれ `0x017a0000`、`0x017a37fc`、`0x017a3900` です。リンク時にコード/データが `0x000a2000` を、BSSが `0x000a3000` を越えると失敗します。この配置でRev.01の実行と停止を確認しました。

PAF1は参考コードのDTI2と別のABIで、64-byte領域内の各wordは機器上では32bitビッグエンディアンです。

| offset | word |
|---|---|
| 0 | magic `0x50414631` (PAF1)、初期化完了時に最後に書く |
| 4 | ABI=1 |
| 8 | heartbeat、各stepで加算。実時間やクロック周波数ではない |
| 12 | hostが要求データの後で公開するrequest sequence、0は無効 |
| 16 | CPUが応答の後で公開するresponse sequence |
| 20 | operation: 1=PING、2=CAPS、3=I²C転送、4=I²C状態と排他診断、5=システム設定の変更と復元、6=固定復調器読み取りの試験、7=診断用I²C転送 |
| 24 | status: 0=OK、1=UNSUPPORTED、2=NOT_READY |
| 28 | features=0 |
| 32..59 | 操作4～7の状態、制御、割り込み保留、割り込みマスク、排他レジスターの初回読み値、システム側設定、排他の再取得結果。その他の応答では0 |
| 60 | 操作5の結果または操作6・7の段階と復元フラグ。操作5は0=試行なし、1=設定・復元一致、2=設定不一致・復元一致、3=復元不一致 |

同じsequenceの要求は再実行せず、初期化時に古い要求・応答を破棄します。単一ホスト・同時1要求の契約で、ホストは前回の応答確認前に次の要求を書かない前提です。I²C転送要求は常にNOT_READYで、状態機械へ転送されません。操作4では、状態・制御・割り込みを32bit、システム側設定と排他レジスターを16bitで読みます。排他レジスターは読み取りで取得するため、bit 0がクリアだった場合だけ0を書いて解放し、再取得できるか確認します。再取得できた場合も直ちに解放します。I²Cバス上の転送は行いません。操作5は排他を取得し、設定が既知の `0x8000` の場合に限り `0x8020` を書いて照合し、直後に `0x8000` へ戻して再照合します。診断ツールでは `--check-clock` を指定します。操作6は `--read-demod` で実行します。Rev.01の既知のコールド状態に限り、Windows側からGPIO bit 1をLow→Highにし、補助CPUで地デジ復調器 `0x10` の `0x0e` を読み取ります。終了時にGPIOの前像も復元します。

volatileとコンパイラーバリアだけでは、CPU間のキャッシュ整合性やUSB経由の可視性を証明できません。コードロード後のキャッシュ/命令同期、共有RAM属性、割り込み状態、ホスト側のreadbackとsequence公開の順序を実機で確認する必要があります。

## Rev.01の補助CPU実機診断

FR向けに既定のPAF1をビルドし、WindowsでWinUSBインターフェイスを指定します。ブート状態と制御レジスターの観測値が実機で確認した前像と一致する場合に実行できます。

```powershell
python aux_probe.py '<WinUSBインターフェイスパス>' build/auxiliary evidence-aux
```

共有RAMの16 KiB、ブート領域、応答用RAM、制御とベクターの観測値を保存します。生成コードの読み戻し後にベクターを設定して補助CPUを起動します。ベクターへの `000a0000` 書き込み後に見える値は `00000000` のままだったため、実行の判定には消去済みメールボックスのPAF1識別、進行するheartbeat、要求番号に対応する応答を使います。終了時は停止ビットとheartbeatの停止を確認してからRAMを戻します。停止を確認できない場合はUSBの抜き差しが必要です。

2026-11-07のRev.01試験では、操作1・2・4の応答と停止・復元を2回連続で確認しました。I²C状態・制御の観測値はともに `00000000` で、WinUSBのメモリー窓から直接読んだ値とも一致しました。共有RAMとブート領域は保存前像と一致し、ベクターの読み値と補助CPU制御の観測値も一致しました。クロック設定と転送完了通知の確認、復調器へのI²C転送は次の段階です。

## I²C状態機械の契約

`dtp_i2c_start` で7bitアドレス、TX/RX各40 bytes以内、操作全体のtimeoutと現在の時刻を渡します。TXはコンテキスト内へコピーされ、非同期動作中の呼出元バッファの寿命に依存しません。RXもコンテキスト内に保持します。

`dtp_i2c_poll` は1回の呼び出しで有限の処理だけを行います。引数 `completion_ready` は、HALが確認して消費する進行/完了通知です。この通知を時刻の経過だけから生成することはできません。通知がなければTX/RXを進めず、操作全体の期限を検査します。**物理通知の正確な条件・acknowledge方法は未実装**で、この引数はモックでの状態遷移と将来のHALをつなぐ境界です。

read32/write32には参考コードで追跡したbaseからのoffset `0xfe0`、`0xfe4`、`0xff0` を渡します。実機のbaseは `0x01684000` と読めますが、このC状態機械は物理アドレスへの直接アクセスを持ちません。

状態はADDRESS_TX→TX→必要ならADDRESS_RX→RX→STOP→DONEです。TX+RXの間にはSTOP要求を挟まず読み取りアドレスを書きます。RXの最終byteと途中byteでは制御bitの扱いを分けます。これらは参考命令列に基づく実験実装で、実際のSTART/STOP/repeated START/ACK波形を確認したものではありません。

時刻はHALから供給されるuint32のマイクロ秒を想定し、開始時からの差分で期限を検査します。timeoutは1..0x7fffffff、時刻のwrapを許容します。pollごとに期限を延長する処理はありません。現在のイメージにはこの時刻を供給する実時間タイマーがないため、I²C実行には使えません。

`owned` コールバックは開始前と進行前に必要です。成立条件は、クロック、リセット、補助CPUの起動状態、他のCPU/公式処理との排他を含み、単にブール値を設定すれば機器上で成立するものではありません。PAF1には所有権を有効化する要求を設けていません。

## 失敗と完了の扱い

- 未準備、引数不正、他の利用者がいる可能性のあるBUSYは、新しい転送を発行せずに返ります。
- 制御器異常はrawのstate/controlとともに保存します。bitの意味を完全に確定していないため、NACKと断定したエラー名へは変換していません。
- timeout/異常の後は再初期化が必要な状態を保持し、次の転送や自動再試行を拒否します。
- 所有権喪失後は状態機械からMMIOを追加発行せず、STOP要求も送りません。所有者の変わったバスへ旧要求を適用しないためです。
- 停止要求bitの操作と、物理STOP/バス回復の成功は別です。`stop_requested` は要求した事実だけを示します。
- `tx_submitted`、進行通知で確認した `tx_completed`、`rx_completed`、`in_flight` を分け、失敗直前のbyteを未確認のまま報告できます。これらの値をバス上のACK確定数にできるかはHALと実機の検証が残ります。
- 状態機械の再初期化は、呼出側が機器静止と回復を確認した後だけが対象です。コード内に未確認のバス回復シーケンスや、参考コードの `0xfe8` writeの自動実行はありません。

## ツールと試験

FR向けGNU GCC 14.2.0とBinutils 2.43.1を使用する構成です。GCCはCフロントエンドだけでよく、対象機器用libc/OSは不要です。

GCC取得先: https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz

tar.xzのSHA-256: `a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9`

ホストのC/C++コンパイラー、make、GMP/MPFR/MPCの開発用ヘッダー・ライブラリーが必要です。Binutilsを構築した後、別のLinux/WSL作業ディレクトリーでGCCのCフロントエンドだけを構築する例:

```sh
mkdir build-fr30-gcc
cd build-fr30-gcc
../gcc-14.2.0/configure --target=fr30-elf --enable-languages=c --without-headers \
  --disable-nls --disable-multilib --disable-shared --disable-threads \
  --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic \
  --without-isl --disable-bootstrap \
  --with-as=/path/to/build-binutils/gas/as-new \
  --with-ld=/path/to/build-binutils/ld/ld-new
make -j4 MAKEINFO=true all-gcc
```

システムへのインストールは不要で、生成した `gcc/xgcc`、`gcc/cc1` とBinutilsを個別に指定できます。

```sh
python3 build_auxiliary.py --out build/auxiliary
```

通常のツール名は `fr30-elf-gcc` と `fr30-elf-{as,ld,objcopy,objdump}` です。名前が異なる場合は `--compiler`、`--assembler`、`--linker`、`--objcopy`、`--objdump` で指定できます。構築直後のxgccを使うときは `--compiler-arg=-B/path/to/gcc-build/gcc/` でcc1の配置を指定できます。manifestは `deployable=false`、features=[]。

ネイティブC試験の例:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
  i2c_engine.c auxiliary.c tests/test_i2c_engine.c -o build/i2c-test
build/i2c-test
```

模擬レジスターで書き込み＋読み取り、入力上限、TXのコピー保持、通知待ち、操作全体の期限、時刻wrap、途中byteの結果不明、STOP待ち中の期限、所有権喪失後のI/O禁止、メールボックスの初期化とsequenceを検査します。これらの試験は制御器の実機仕様を検証するものではありません。

`tests/test_auxiliary_image.py --build build/auxiliary -v` は、GCCが生成したFRコードのメールボックス初期化・要求応答関数を限定命令モデルで実行します。FRの遅延分岐にも対応する範囲で、magic/ABI、不正identity、sequenceの再実行拒否、I²C未準備、応答sequenceを最後に公開する順序を確認できます。割り込み・キャッシュ・実機MMIOはモデルの対象外で、I²C状態機械のFR機械語を全経路実行する試験ではありません。

次の段階では、実時間タイマーと進行通知のHAL、補助CPUの安全な起動/静止・復元、クロック/リセット、CPU間の所有権を検証し、読み取り中心の実機試験につなげる必要があります。

### 診断用I²C転送

操作7の補助バッファはメールボックス直後のCPU `0x000a3940` / USB `0x017a3940`。offset 0～3は7bitアドレス、TX長、RX長、予約0。offset 4～11はTX、12～19はRX。Rev.01で根拠があるアドレス `0x10` / `0x11` とTX・RX各8 byte以内に制限する。ホストはバッファを先に書き、最後に要求シーケンスを公開する。応答シーケンスと転送段階・復元フラグを確認してからRXを読む。転送失敗時は同じ要求を再送せず、ローダーの終了処理へ進む。機器側の待ちには固定のポーリング回数上限を使い、実時間の期限とは区別する。

```powershell
python aux_probe.py '<WinUSBインターフェイスパス>' build/auxiliary evidence-i2c --transfer 0x10:0e:1 --transfer 0x10:0e:2
```

各要求は同じGPIO・補助CPUセッション内で実行し、制御器設定はトランザクションごとに復元する。受信機のI²Cレジスターへの書き込みは診断の指示どおり実行される。終了時の復元照合対象はGPIO、システム設定、補助CPUの制御と共有RAM、ブート領域であり、復調器・RFチューナーの全レジスターの前像比較は含まない。
