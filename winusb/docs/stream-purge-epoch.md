# TS のパージと世代の対応

BonDriver の `PurgeTsStream` で捨てたはずのパージ前の TS が、パージ後に利用側へ渡る競合と、その対処の記録です。
BonDriver と DriverHost の間の手順を変えたため、理由、維持した契約、移行の扱い、試験の範囲を残します。

## 現象

MSi2500 の J.83B を BonDriver 経由で選局すると、x86 版で 3 回の試験のすべてで、選局直後の出力の先頭から 35〜460 パケット目に、
9〜17 個の PID の CC が 1 回ずつ飛びました。x64 版でも、新しくビルドした helper を初めて起動した選局で 1 回起きました。
helper の出力に誤りはなく、飛ぶ前の TS も新しい搬送波のものでした（`msi2500-j83b-design.md` の 2.7 節）。
`BonDriver_MSI2500.ini` の `NumberOfBuffersToIgnoreAfterPurge` を 1 にして、この現象を避けていました（#39）。

## 原因

`BonDriver::SetChannel` は選局の後に `PurgeTsStream` を呼びます。`PurgeTsStream` はデータのパイプへ `DataCmdCode::PURGE` を書き、
DriverHost の応答を待たずに `IoQueue::PurgeDataBuffer` で受信キューを空にしていました。このとき、次の 2 か所のパージ前の TS が残ります。

1. `IoQueue::ReadWorker` が読み込み中のバッファー。`PurgeDataBuffer` は完成したバッファーの列だけを空にし、読み込み中のバッファーは
   パージの後に完成して列へ入ります。
2. DriverHost がパージを処理する前に送り、まだパイプの中にあるデータ。

その後に DriverHost のパージで間を捨てた TS が続くため、境目で CC が飛びます。`FramedStream=true` の接続はフレームに世代（epoch）を
載せていますが、パージは世代を変えないため、BonDriver はパージ前のデータを区別できませんでした。

選局が TS の到着を待ってから終わる系統（ISDB-C と J.83B の `TuneIsdbC`、ロックを待つ ISDB-T・ISDB-S）では、`PurgeTsStream` の時点で
TS が流れているため、この競合が起こりえます。MSi2500 と RTL-SDR の ISDB-T は選局の受付で戻り、最初の TS まで約 2 秒あるため、起きにくい構成です。

## 設定との関係

`NumberOfBuffersToIgnoreAfterPurge` は、パージの後に届くバッファーを指定の数だけ捨てる設定で、上の 1 を捨てる効果があります。
パイプの中のデータが 2 バッファー以上あるときは防げず、パージ後の正しい TS も 1 バッファー分捨てます。配布の ini の値は次のとおりです。

| 値 | ini |
|---|---|
| 1 | DT295、ISDB2056、ISDB2056N、ISDBT2071、MSI2500（#39）、PX-M1UR、PX-MLT、PX-S1UR、Torne、XIT-SQR100、XZ3 |
| 0 | PX-BCUD、PX-S1UD、PX4-S、PX4-T、RTLSDR |

PX4-T・PX4-S を 0、PX-MLT を 1 にしたのは原著者の `920fa75`（2021-07-18「微調整」）で、理由の記録はありません。

## 対処

`FramedStream=true` の接続に限り、パージで世代を進め、BonDriver がパージより前の世代のデータを捨てるようにしました。

1. 制御のパイプに `CtrlCmdCode::PURGE_STREAM`（40）を追加しました。DriverHost は受信機の `StreamBuffer::AdvanceEpoch()`
   （リングを空にして世代を進める既存の処理）を呼び、進めた後の世代（32 ビット、0 以外）を `CtrlPurgeStreamCmd::epoch` で返します。
   データのパイプのフレームは読み出した時点の世代を載せるため、パージ後のデータは必ず新しい世代になります。
2. BonDriver の `PurgeTsStream` は、`FramedStream=true` のとき `PURGE_STREAM` を呼び、返った世代 N を `IoQueue::PurgeDataBuffer(N)` に渡します。
   `IoQueue` は N より前の世代（32 ビットの差を符号付きで比べ、負なら前）のデータを捨てます。受信スレッドはパイプから読んだ古い世代のバイトを
   読み捨て、`PushBackDataBuffer` は古い世代だけを持つバッファーをキューへ入れません。後者の判定は、`PurgeDataBuffer` が N を設定するときと
   同じ `data_mtx_` の中で行うため、キューを空にした直後に受信スレッドが追加する分も捨てられます。N 以降の世代は従来どおり受け付けます。
3. `PURGE_STREAM` が失敗した場合（このコマンドを知らない古い DriverHost は `FAILED` を返す）は、データのパイプの `PURGE` と
   `NumberOfBuffersToIgnoreAfterPurge` に戻します。

## 維持した契約

- `FramedStream=false` の接続、データのパイプの `PURGE`、`NumberOfBuffersToIgnoreAfterPurge` の意味は変えていません。
- コマンドの版（`command::VERSION`）は変えていません。`PURGE_STREAM` への対応は応答で判断するため、`FramedStream` の版の確認
  （`0x00040004` 以降）はそのまま使えます。
- 古い BonDriver と新しい DriverHost、新しい BonDriver と古い DriverHost の組み合わせは、従来の手順で動きます。
- 世代の値は 0 を使いません（`AdvanceEpoch` の既存の扱い）。
- 配布の ini の `NumberOfBuffersToIgnoreAfterPurge` は変えていません。新しい組み合わせでは、世代による除外の後に従来の除外も働くため、
  1 の ini ではパージ後の正しい TS を 1 バッファー余分に捨てます。0 へ戻すかは、機種ごとに実機で確かめてから決めます。

## 試験

### 自動試験

`tests/stream_epoch_test.cpp` に次を加えました（x64 と x86 で成功）。

- パージの前に完成してキューにあったバッファー、パージの時点で読み込み中だったバッファー、パイプに残っていた古い世代のデータが
  渡らず、パージ後の世代が最初に渡ること。その後の選局で進んだ世代も受け付けること。
- 世代の 32 ビットの折り返し（0xffffffff でパージした後の 1 を受け付け、0xfffffffe を捨てる）。
- 世代を指定しないパージ（古い DriverHost との組み合わせ）では、世代で捨てないこと。
- `AdvanceEpoch` が進めた後の世代を返し、続けて呼ぶと 1 ずつ進むこと。

### 実機（2026-11-17）

Windows 11 x64、RSP1 互換機（MSi2500）と PX-S1UD、`tests/msi2500_bondriver_live.ps1` と `rtlsdr_bondriver_live`。

| 試験 | 結果 |
|---|---|
| MSi2500 x86 J.83B、`NumberOfBuffersToIgnoreAfterPurge=0`、H008↔H007 | 選局 6 回で選局直後の CC の飛びは 0（変更前は 3 回の試験のすべてで発生） |
| MSi2500 x64 J.83B、同じ ini | 選局 4 回、誤り 0 |
| MSi2500 x64 J.83B、新しい BonDriver と変更前の DriverHost | 選局 3 回、誤り 0（`PURGE` へ戻る） |
| MSi2500 x64 J.83B、変更前の BonDriver と新しい DriverHost | 選局 3 回、誤り 0 |
| PX-S1UD x64 J.83B（ini は 0）と ISDB-T | J.83B の選局 6 回、ISDB-T の選局 2 回、いずれも誤り 0 |

MSi2500 x86 の 2 回の試験では、受信の途中（選局から約 1.5 秒後と 16.8 秒後）に CC が飛びました。いずれも helper が記録した USB の
通信エラー（`USBD_STATUS_XACT_ERROR`、1 パケット分の欠落）の位置で、パージの競合ではありません。
MSi2500 を USB ハブの先のポートへつないだ構成では、等時転送の多くが失敗して受信できなかったため、ルートハブのポートで試験しました。

## 対象外

`FramedStream=false` の機種（PX4 系、PX-MLT 系、ISDB2056 系、XIT-SQR100 など）は世代を持たないため、変わりません。
これらの機種で同じ競合が起きているかは確かめていません。BonDriver の手順は従来と同じで、自動試験の「世代を指定しないパージ」で確かめていますが、
これらの機種の実機の試験はしていません。
