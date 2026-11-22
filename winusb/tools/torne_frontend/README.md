# CECH-ZD1J の復調器 (DiB8096) の Go 実装

このディレクトリーは、coax の DiB8000・DiB0090 の Go 実装 (`internal/coaxd/demod/dib8096`) と、それを C++ へ変換する道具を保持します。  
DriverHost_PX4 と Linux 版のデーモンは、ここから生成した `src/DriverHost_PX4/torne/dib8096_native_generated.inc` を使うため、通常のビルドと実行に Go は不要です。  
Go のソースは、生成済みの C++ の生成元 (GPL の対応ソース) と、C++ の動作を確かめる比較の基準として残しています。  
構成と経緯は [ネイティブ復調器](../../docs/cech-zd1j-native-frontend.md) と [Linux 対応の設計](../../../docs/20261121_torne_linux_cuse_design.md) の「Go への依存」を参照してください。

## 生成と比較

| 道具 | 用途 | Go |
|---|---|---|
| `port_native.go.txt` | Go の型情報付き AST から `dib8096_native_generated.inc` を生成する変換器 | 必要 |
| `tests/check_torne_native.py` | Go の模擬バスと C++ の I²C の要求列、待機、結果を 9 条件で比べ、リポジトリの正解の記録 (`tests/data/torne_native`) が最新かも確かめる | 必要 |
| `torne_native_frontend_test --golden` | 正解の記録と C++ を比べる。`build.ps1` が x86 と x64 で毎回実行する | 不要 |
| `build_comparison.py` | 元の coax の USB 経路も使う独立の比較用の実行ファイルを作る。比較対象のチェックアウトは変更しない | 必要 |

```text
python winusb/tests/check_torne_native.py --arch x64
python winusb/tests/check_torne_native.py --arch x64 --regenerate
python winusb/tests/check_torne_native.py --arch x64 --update-golden
```

`--regenerate` は変換器で `dib8096_native_generated.inc` を作り直し、`--update-golden` は Go が作った記録を `tests/data/torne_native` へ書き戻します。  
Go の実装や変換器を変えた場合は、両方を実行して差分を確かめてから、生成物と記録を同じ変更へ含めます。

選局コマンド `0x11` は、元の探索状態が `statusTuneFailed` かつ `demodStop` で確定終了した場合だけ、本文 1 バイト `01` を返します。  
探索中の未ロックや信号レベルでは打ち切らず、無信号のチャンネルの判定を速めるためだけに使います。  
AGC の起動の待機は 16・14・16 ms です (詳細は `LOCAL-CHANGES.md`)。追加のアクセサーは `tune_result.go` にあります。

## 削除した診断ヘルパー

以前は、Go の復調器を子プロセスとして動かし、標準入出力で I²C を仲介する診断ヘルパー (`cmd/torne_frontend`、`TorneFrontendDiagnostic.exe`) がありました。  
通常版がホスト内の C++ 実装へ移り、C++ と Go の比較も上の試験で行えるようになったため、ヘルパーと、それを使う診断 (`torne_usb_probe` の `--frontend`、`--receive`、`--receiver`、`--receiver-recovery`、`tests/torne_reference_client.*`、`tests/torne_receiver_adapter.*`、DriverHost 側の `torne/frontend_client.*`) は削除しました。  
過去の検証記録に出てくるこれらの名前は、当時の構成を指します。  
`torne_usb_probe` には、記述子の確認、ブリッジの起動 (`--initialize`)、復調器の識別 (`--identify`)、カードの状態 (`--card-status`、`--card-atr`) の診断が残っています。

## 個体の選択

`python winusb/tests/build_torne_identity.py` は、2 つのプロジェクトだけをビルドし、USB へアクセスしない基盤試験を実行します。  
引数なしの `torne_usb_probe` は読み取りだけで、各 interface の 16 桁の ID と記述子を表示します。  
診断の引数の末尾へ `--device-id <16桁ID>` を加えると、列挙の順序に依存せず 1 台を選びます。  
初期化を伴うモードは、ID を指定せずに複数台が見つかった場合や、同じ ID が複数ある場合には拒否します。  
ID は大文字小文字を正規化した interface path の FNV-1a の表示値で、製造番号ではありません。  
USB ポートの変更や OS によるデバイスの再登録で変わる可能性があり、接続されていない ID から別の個体へ自動で移ることはありません。  
この選択の経路の複数台での実機動作と、別の個体の同時受信は未確認です。

## ライセンス

出自とハッシュは `upstream.json`、著作権とライセンスは `NOTICE`、`COPYING` などを参照してください。  
GPL 側の条件を選択しています。  
`GO-LICENSE` は、Go で作った実行ファイル (`build_comparison.py` の出力など) へ静的にリンクされる Go の実行時ライブラリーのライセンスで、そうした実行ファイルを配布する場合に同梱します。
