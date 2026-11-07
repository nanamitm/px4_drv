# PIX-DT295のソースからビルドするファームウェア

このディレクトリーは、PIX-DT295のI²C制御をホスト側へ置く構成を調査・開発するための実験実装です。現在ビルドできるものは **FR向けのプロトコル処理コア、診断用ブートアダプター、補助CPU向け診断イメージ** です。PIX-DT295 Rev.01ではプロトコル処理コアのRAM実行と復元を確認しました。生成manifestの `deployable` は `false` です。

`resident.s`、`bootstrap.s` と各リンカースクリプトからコードを生成します。ビルド入力にmAgicVISION、pix-userland、公式ファームウェアのバイナリは含まれません。公開ホストコードとバイナリの静的解析から得た配置情報・命令セットの知識を使った実装であり、クリーンルーム開発を証明するものではありません。

## 役割と現在の境界

目標となる構成は次の通りです。

```text
BonDriver / DriverHost
  ├─ 機種別の初期化・選局・ロック確認・TSID選択
  ├─ PIF1要求/応答、絶対期限、切断時の状態破棄
  └─ WinUSB
       └─ ソースからビルドする機器側コード
            ├─ USB要求受理・応答、対象機種/ABI確認
            ├─ I²C単一/複合転送、NACK・期限切れの通知
            └─ 必要なTS取り込み・USB bulk転送
```

現時点で実装されているのは、整列した64-byteパケットを受け取るプロトコルコア、既存USBブート処理からコアへ接続する診断用アダプター、ホスト用のパケットコーデックです。コアは能力照会と疎通確認に応答し、I²C要求にはNOT_READYを返します。能力ビットは0で、I²C/TSを利用可能とは通知しません。機器レジスターや任意メモリーを読み書きするコマンドはありません。

DT295向けUSBアダプターは実機でCAPS/PINGと復元を確認済みです。物理HAL、電源/リセット、TS取り込み・USB転送、DriverHostとの接続は未実装です。通常ビルドとパッケージングには含まれません。

I²CのC状態機械と、補助CPUのC診断プログラムを追加しています。既定のPAF1はDT295 Rev.01の補助CPUで実行し、物理I²C制御器の状態レジスターを読めます。I²C転送の状態機械への接続は残っています。ビルド・実機診断・ABIは [auxiliary-design.md](auxiliary-design.md) にあります。torne用の別ビルドPAF2では、ソース生成FRイメージを実機で動かし、HAL経由のID読み取りと100組の反復一致、選局・80秒のTS受信、core1 RAMの復元・既存経路への復帰を確認しています。時計は診断用のホスト供給方式です。詳細は [torne-fr-i2c.md](torne-fr-i2c.md) にあります。

## ビルド

Python 3.10以降と、Fujitsu FR30向けGNU Binutilsが必要です。生成した機械語は、下記の診断経路でDT295 Rev.01上の実行を確認しました。

```sh
python3 build.py --out build
python3 tests/test_core.py --binary build/resident.bin -v
python3 tests/test_bootstrap.py --build build -v
```

標準のツール名は `fr30-elf-as`、`fr30-elf-ld`、`fr30-elf-objcopy`、`fr30-elf-objdump` です。名前や配置が異なる場合は `--assembler`、`--linker`、`--objcopy`、`--objdump` で個別に指定できます。Pythonとツールが同じファイルパスを参照できる環境で実行してください。

検証に使ったGNU Binutils 2.43.1の取得先:

https://ftp.gnu.org/gnu/binutils/binutils-2.43.1.tar.xz

tar.xzのSHA-256: `13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd`

Linux/WSLでのツール構築例:

```sh
mkdir build-fr30
cd build-fr30
../binutils-2.43.1/configure --target=fr30-elf --disable-gdb --disable-gdbserver \
  --disable-gprofng --disable-gprof --disable-gold --disable-nls \
  --disable-werror --without-debuginfod
make -j4 MAKEINFO=true all-gas all-ld all-binutils
```

構築直後のツールは `gas/as-new`、`ld/ld-new`、`binutils/objcopy`、`binutils/objdump` です。生成される `resident.elf`、`resident.bin`、`bootstrap.elf`、`bootstrap.bin`、`boot-hook.bin`、逆アセンブル結果、ソースハッシュ付きmanifestは `--out` の下に置かれます。rawイメージはSBC2形式ではなく、現在のDriverHost用ファームウェアと交換しても動作しません。

## PIF1 ABI 1

これは機器の既存プロトコルとは異なる開発用ABIです。診断用USBアダプターと実機での確認手順は次節にあります。

パケットは64 bytes、整数はビッグエンディアンです。

| offset | 要求 | 応答 |
|---|---|---|
| 0 | magic `0x50494631` (PIF1) | 同じmagic |
| 4 | sequence | 要求のsequence |
| 8 | operation | operationに `0x80000000` をOR |
| 12 | ABI=1 | status |
| 16 | 将来の転送timeout_us、通常0 | ABI=1 |
| 20 | 将来のI²C selector、通常0 | 応答payload長 |
| 24..63 | payload、余白は0 | payload、余白は0 |

operationは1=CAPS、2=PING、3=I2C。statusは0=OK、1=BAD_MAGIC、2=BAD_VERSION、3=UNSUPPORTED、4=NOT_READYです。CAPSは4個の32bit値（機能ビット=0、開発中の最大TX=40、最大RX=40、対象USB ID=`0x06b8103b`）を返します。このIDはコアの対象を示す固定値で、実機のID検査に成功したという意味ではありません。PINGとエラーのpayload長は0です。

ホストコーデックが準備する将来のI²C要求は、bus=0の7bitアドレス、TX/RX各40 bytes以内、少なくとも一方が非空の1トランザクションです。selectorは `address << 16 | tx_length << 8 | rx_length`、TXデータはpayloadです。TX+RXではrepeated STARTを保つ複合転送を想定しています。ただしコアにはI²Cの引数検査・期限管理・転送実装がまだないため、現段階では常にNOT_READYになります。timeout_usの有効範囲・タイマー・丸め規則はバックエンド確定時の仕様です。

今後、NACK、期限切れ、バス異常、切断、結果不明の書き込みを区別するため、statusと完了バイト数の仕様が必要です。途中まで進んだ書き込みの自動再試行は同じ操作を二度適用する可能性があるため、回復手順の実機検証と合わせて設計する必要があります。

コア入口は `dtp_entry`、リンク先はブート用RAM内の `0x000e1100` です。呼び出し契約はr4に4-byte整列済み・読み書き可能な64-byteパケット、r0..r3と条件コードを破壊可能、r4..r15とRPを保持、非再入です。スタックとBSS初期化は不要です。パケットの領域確認・排他・CPU状態の整備はUSBアダプター側の責務で、アドレスを外部入力から直接受け取る設計にはしていません。受信中の割り込みとの共存は未検証です。

## 診断用ブートアダプター

`bootstrap.s` はDT295の既存BC OUTハンドラーの `0x000ea6fc` へ8-byteフックを置き、`0x000e1000` のコードへ分岐する構成です。r11が指す既存要求構造体のselector（offset `0x46`）= `0x7e`、length（offset `0x48`）=64、転送先（offset 0）=`0x000c4620` の場合だけ、構造体+4の受信データをstagingへコピーしてコアを呼びます。その他は元のmask/通常書き込み処理への分岐を再現します。応答はstagingに残り、既存のEP0完了処理 `0x000ea8c8` へ戻ります。

この仕組みは機器にある既存USBブート機能を使います。ソースから作る範囲は、フック、アダプター、常駐コアであり、機器の全ROM/既存ブートプログラムを置き換えるものではありません。

`boot_contract.py` は既知のDT295ブートスナップショット（`0x000e9000` から `0x3910` bytes）のSHA-256、フック前像、生成物のハッシュ・配置・サイズを検査するオフラインツールです。USB APIは含まれず、出力にも `executable=false` が付きます。診断用のOUT/IN要求値と、前像保存→コード検証→フックを最後に有効化する意図を記録できますが、CPU静止、途中失敗、復元の実装と検証を代用するものではありません。

```sh
python3 boot_contract.py --build build --boot-snapshot /path/to/dt295-boot-snapshot.bin
python3 tests/test_contract.py --build build --boot-snapshot /path/to/dt295-boot-snapshot.bin -v
```

## WindowsでのRAM診断

PIX-DT295をWinUSBで接続し、受信ソフトから対象機器を閉じて実行します。インターフェイスパスは `pnputil /enum-devices /instanceid <インスタンスID> /interfaces` で確認できます。生成物とログ用ディレクトリーの引数はWindowsから見えるパスです。

```powershell
python boot_probe.py '<WinUSBインターフェイスパス>' build evidence-read
python boot_probe.py '<WinUSBインターフェイスパス>' build evidence-run --execute
```

既定ではUSB記述子・ブート状態・既知ブート前像の確認だけを行います。`--execute` は実機で確認済みのブート状態 `0000` を要求します。RAM前像を保存してから、コアとアダプターを書き込み、読み戻しで照合した後にフックを有効にします。CAPS/PINGの応答ではシーケンス番号、ABI、機種IDと能力を確認します。終了時はフックを最初に戻し、BCブロック書き込みが作業領域を使うため応答バッファを最後に戻します。保存した範囲のバイト一致とブート状態の一致がそろったときに `restore=verified` を出します。USB通信が停止し復元も失敗した場合は、対象機器をUSBから抜き差ししてブート状態を再確認してください。

Rev.01の実機では、コアを外部側の `0x80380000` に配置した初回試験で、書き込み後の照合は成功し、CAPSの応答読み取りとフック復元がタイムアウトしました。USBの再接続後に既知ブート前像の一致を確認し、コアをブート用RAM内へ移すとCAPS/PINGが成功しました。最終版で、CAPS/PINGとRAM・ブート状態の復元をUSBの再接続を挟まず2回連続で確認しました。ホスト側で受信済みのCAPS応答のmagicを破損させた試験でも、異常を検出した後にRAMとブート状態が一致しました。

## 検証

`tests/test_core.py` は生成されたFR機械語を、使用命令だけに対応した限定命令モデルで実行します。命令モデルの根拠はGNU Binutilsの `cpu/fr30.cpu` です。コア単体では全ロード/ストアを64-byteパケット内へ制限し、未対応命令、領域外アクセス、無限実行、保持レジスターの破壊を失敗にします。

能力通知、sequence保持、未実装I²C、未知コマンド、不正magic/ABI、任意payloadを含む500ケース、NULL入口、ホスト側の古い応答・長さ不正・余白破損の拒否を検査します。このモデルにはUSB、割り込み、キャッシュ、MMIO、バス、実機タイミングがないため、合格は機器上での実行成功を示しません。

`tests/test_bootstrap.py` はリンク済みフック→アダプター→コアを同じ命令モデルで実行し、CAPS/PING、古いstagingを使わないこと、不正パケットへの応答、対象外要求の元処理への分岐、staging外へ書き込まないことを確認します。既存EP0完了処理の入口で実行を止めるため、その先のハードウェア動作の確認ではありません。

参考バイナリに含まれる補助CPUコードの抽出は、独立した解析ツール `tools/extract_reference_aux.py` で再現できます。これはファームウェアビルドの入力にはなりません。抽出時は固定版バイナリのSHA-256、展開長、チェックサムを検証します。解析結果と未解決点は [research.md](research.md) にあります。

```sh
python3 tools/extract_reference_aux.py --reference /path/to/magicvision-dt295.bin --out /path/to/analysis
python3 tests/test_reference_aux.py --reference /path/to/magicvision-dt295.bin -v
```

既存WinUSBソリューションの実行経路を変更していない段階では、このコアのビルドと命令モデル試験が対象です。DriverHostへ接続する段階ではx86/x64のビルド・転送試験と、受信中のTS同期・TEI・CCの検証が必要になります。

補助CPU向けCは `build_auxiliary.py` で別にビルドします。`tests/test_auxiliary_image.py --build build/auxiliary -v` はGCCが生成した実際のFR命令でメールボックス初期化・応答・sequence保持を確認します。I²C状態機械そのものはネイティブCの模擬I/O試験が対象で、生成したFR版I²C処理の実行・物理転送を確認したものではありません。

## 次の実装段階

torneで最下層を検証するためのHALは [torne-hal.md](torne-hal.md) にあります。Windows上の専用診断ツールと、[新FR版のPAF2診断](torne-fr-i2c.md) の双方で、ID読み取り・100組の反復一致、GR13→GR17→GR13の選局と80秒のTS受信を確認しています。各試験では初期同期のバイト破棄と、同期後に破棄・TEI・CCエラーがなかった区間を分けて記録しています。

1. **I²Cの最下層の特定**: コントローラーのMMIO、クロック/リセット、開始/停止/repeated START、完了条件、NACK、タイマー、バス回復を根拠付きで記録。既存常駐の呼び出しとキュー/補助CPU依存も分ける。
2. **ソースを持つUSB/ブートアダプター**: 診断用アダプターはビルド・命令モデル試験まで実装済み。Rev.01で入口とロード/復元を確認済み。既存バイナリのブートストラップや常駐イメージの流用をビルド条件にしない。
3. **実機でCAPS/PING**: 機器識別とABIを照合し、ブート前後の状態・復元を確認。ここで機械語モデルと実機の差を検証する。
4. **読み取り中心のI²C検証**: 相手機器と副作用のないレジスターを資料で選び、既知の値と比較。読み取りでもポインター指定の書き込みが必要な場合があるため、取引全体の影響を確認する。
5. **ホスト側の制御実装**: 電源・リセット、初期化、地上波選局から段階的に実装し、衛星選局を追加。既存のtorne経路の正常動作はDT295固有の証拠にはならない。
6. **TS経路**: 取り込み、DMA/補助CPU、リング管理、bulk転送、停止/再開、切替時の世代を実装。短時間受信・切替・再オープン・抜去・長時間受信の順にsync/TEI/CC/空受信時間を確認する。

完成条件は、全実行コードを保存済みソースから再生成でき、I²CとTSの必要経路について不透明な常駐バイナリに依存せず、実機で制御・受信・回復が検証されていることです。内蔵カードとMULTI2処理は初期段階の対象外です。

### USB Bulkの診断ビルド

`build.py --bulk` はソースから生成する常駐コードにPIF1操作4を追加します。通常のBinutils指定に加え、補助CPUビルドと同じFR向けGCCを `--compiler` / `--compiler-arg` で指定します。動作には、ホスト側で独立してTSエンジンを設定・停止し、同じWinUSBハンドルでBulk IN `0x81` を読む診断用オーケストレーションが必要です。機器側の操作仕様と実測結果は [research.md](research.md) にあります。

同じWinUSBハンドルでの再選局と定常受信は `bulk_validate.py` で測定できます。PAF1の補助CPUビルド、`build.py --bulk` の出力、対象機の地デジ初期化JSONL、未作成の証跡ディレクトリを指定します。JSONLは比較調査で抽出した地デジ初期化列を使います。`op` と、操作に応じて `payload`（16進）または `delay_ms` を持つ行形式です。

```powershell
python bulk_validate.py $interface $auxiliaryBuild $bulkBuild $initTrace $evidence --channels 13:31761 17:31760 13:31761 --seconds 5 --long-seconds 1800
```

TSIDは受信地域の値を指定します。各短区間のPATを照合してから次へ進み、最後に同じ選局のまま固定受信します。選局中はTSエンジンを停止し、次の区間で起動します。`analysis.json` はTSエンジン起動後1秒の前後を分け、PATのTSID、同期、TEI、CC、形式異常、USB読み取りの空白時間を記録します。想定外の有効PATは起動直後でも失敗として扱います。PATを含まないパケットの送出元までは判定できません。`bulk-timing.csv` は64 KiB単位のUSB完了時刻です。30分で約7 GBの生データを保存します。RAM・リング等の復元結果は標準出力に記録します。

### BS/CS選局

同じ診断経路で `BS01:16400` や `CS02:24608` のように物理チャンネルとTSIDを指定できます。BSは奇数の01～23、CSは偶数の02～24です。衛星を指定すると、地デジ初期化に続いて `op=2` の衛星初期化を含むJSONLが必要です。衛星復調器のロックと選択TSIDを読み戻した後、受信したTSのPATでもTSIDを照合します。アンテナは外部から給電されている環境が前提です。

```powershell
python bulk_validate.py $interface $auxiliaryBuild $bulkBuild $initTrace $evidence --channels BS01:16400 CS02:24608 13:31761 BS01:16400 --seconds 5
```
