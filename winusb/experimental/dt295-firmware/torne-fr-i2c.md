# torneで動かすソース生成FR I²C診断

対象はCECH-ZD1Jの補助CPU core1です。`torne_auxiliary.c`、`torne_hal.c`、`i2c_engine.c` と既存のCランタイム・FR入口から専用イメージを生成します。Windows側はI²Cレジスターを直接進めず、共有RAMのPAF2要求を介してFR側のHALを実行します。DT295用のPAF1診断イメージ・USBコアとは別の実験ABIです。

2026-11-05、Windows x64 / CECH-ZD1JでPAF2起動、PING/CAPS、I²C初期化、DiB8096のvendor ID `01b3` / device ID `8002`、100組の反復一致を確認しています。今回の新FR版での選局・TS受信は次段階です。先行するWindows上のHALによるTS受信結果とは区別します。

## 配置とビルド

CPU側入口は `0x000a0000`、スタック上端は `0x000a37fc`、共有RAMのメールボックスはCPU `0x000a3900` / ホスト `0x017a3900` です。コード/データ上限 `0xa2000`、BSS上限 `0xa3000` は既存と共通です。最終実機試験のイメージは4,360 bytes、SHA-256は `e663ff8750bab874939a475d0d4dd6530d139421904d53c533a254b04776dce0`。

```sh
python3 build_auxiliary.py --torne-i2c --out build/torne-i2c
cc -std=c11 -Wall -Wextra -Werror -pedantic -DDTP_TORNE_AUX_TEST -I. \
  torne_auxiliary.c torne_hal.c i2c_engine.c tests/test_torne_auxiliary.c -o build/mailbox-test
build/mailbox-test
```

FR GCC/Binutilsの指定は [auxiliary-design.md](auxiliary-design.md) と同じです。`--with-torne-hal` だけなら従来どおり未接続のHALをリンクしたPAF1診断、`--torne-i2c` ならHALを実行するPAF2診断になります。ビルド入力に既存ファームウェアバイナリは含まれません。manifestは対象をCECH-ZD1J、診断種別を `torne-paf2-host-clock` と記録し、通常配布への接続がないため `deployable=false` を維持します。

## メールボックス

全192 bytes、32bit整数はBE、TX/RXはbyte配列です。単一ホスト、同時に1要求だけを処理します。

| offset | 内容 | 書き込み元 |
|---|---|---|
| 0 / 4 / 8 | magic `50414632` (PAF2) / ABI=2 / heartbeat | FR |
| 12 / 16 | request sequence / response sequence | ホスト / FR |
| 20 / 24 / 28 | operation / status / features | ホスト / FR / FR |
| 32 / 36 / 40 | host_us / lease token / request token | ホスト |
| 44 / 48 / 52 / 56 | 7bitアドレス / TX長 / RX長 / timeout_us | ホスト |
| 60 | HAL result (`dtp_i2c_result`) | FR |
| 64..103 / 104..143 | TX最大40 bytes / RX最大40 bytes | ホスト / FR |
| 144 / 148 | ACK確認済みTX数 / 成功時RX数 | FR |
| 152 / 156 / 160..191 | 最終state / control / 予約領域 | FR |

operationは1=PING、2=CAPS、3=I2C、4=PREPARE、5=QUIESCEです。statusは0=OK、1=UNSUPPORTED、2=NOT_READY、3=BAD_REQUEST、4=TRANSFER_FAILED、5=CLOCK_STALLED、6=REVOKED。featuresはbit0=I²C準備済み、bit1=ホスト供給時計です。旧PAF1やPIF1のstatusとの互換性はありません。

ホストはmetadataとTX、operationを先に置き、request sequenceを最後に公開します。FRは要求をコピーし、sequenceとleaseを再確認します。完了時はRX・結果・カウンターを公開してからresponse sequenceを最後に書きます。同一sequenceを再実行せず、転送開始後の異常ではHALを再利用禁止にします。RXは成功時だけ採用でき、失敗応答では全RXと完了RX数を0にします。TX投入済みbyteの適用有無はACK確認数だけでは確定できません。

PREPAREには非0で一致するlease/request tokenと、ホスト時計の更新観測が必要です。排他USBハンドル、旧core1の停止、既存のクロック設定はローダーが先に確立します。leaseだけで物理的な排他が成立する仕組みではありません。

## 時計と中断

MB86H47の自走マイクロ秒タイマーは未確定です。本診断はWindowsのsteady clockを32bitマイクロ秒で共有RAMへ書き、FRがその値を期限判定に使います。更新間隔にはUSB往復とホストスケジューリングが含まれ、マイクロ秒精度の期限保証ではありません。

FRは転送開始時からのuint32差分で固定期限を判定します。別に、時計更新なしで `0x40000` 回pollした場合はCLOCK_STALLEDとして打ち切ります。この回数は時間換算や待機時間ではなく、ホスト停止時の有限処理上限です。leaseが残る場合だけSTOPを要求し、転送を再利用禁止にします。lease喪失後は追加のMMIOを発行せず、ローダー側のCPU停止と回復が必要です。

## 診断ローダーと復元

```powershell
MSBuild.exe torne_aux_probe.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
.\build\aux-probe\torne_aux_probe.exe --run '<exact torne interface path>' `
  '.\build\torne-i2c\auxiliary.bin' '<expected SHA256>' '<backup prefix>'
```

イメージのサイズと指定SHA-256を確認してから、対象VID `054c` / PID `04b2` を共有なしで開きます。DriverHostが未使用のtorneを保持している場合も、解放されるまでは失敗終了します。実行中の受信を自動停止する機能はありません。

1. 既存のソース生成bootstrapでアドレス拡張とクロックを設定し、既存core1を停止します。
2. core1 RAM `0xa0000..0xa3fff` の16KBを読み、ファイルへ保存してから新イメージをロードします。全アップロードを読み戻して一致を確認し、PAF2領域を消去して起動します。
3. PAF2 magic/ABI、PING/CAPS、既存の復調器リセット、PREPARE、enumeration、ID読み取りを実行します。IDは初期確認後に100組を比較します。
4. QUIESCE、core1停止、I²C STOP後のidleを確認し、保存RAMを書き戻します。RAM全体を再読して完全一致を確認し、復元後の内容もファイルへ保存します。
5. 既存bootstrapが設定した起動ベクター `0xa0000` を再設定し、旧core1のハートビート再開を確認します。

起動ベクターレジスター `0x017ffffc` は実機では設定値をそのまま返さず0と読めました。読めた0を保存前像として書き戻すのではなく、直前の `Bootstrap::Initialize` が設定した既知の値を復元コマンドとして保存します。ここでの確認はRAMの完全一致、ベクター設定要求の成功、既存core1の実行再開です。読み返しだけによるベクター一致の証明ではありません。

この復元の基準は「既存bootstrapで初期化した後のcore1」であり、機器全体の電源投入前像ではありません。初期化・停止・復元・再開が失敗した場合、診断は非0で終了しUSB再接続が必要な旨を表示します。プロセスクラッシュや抜去後の自動復元を保証する機能はありません。

## 確認した範囲

最終イメージでPAF2要求208件、I²C転送204件（enumeration 1回、初期ID 3回、反復ID 200回）が成功しました。復元前後のRAM SHA-256はともに `87e97f2366f944de2e731e2dddd25c261ff10529018c42c6016f3483e6f9e70f`。旧core1のハートビート再開と、従来のメールボックス経路でvendor `01b3` / device `8002` を再取得できることも確認しています。

ネイティブの同一サービスコードでは、lease/sequenceの失効、通知前の進行禁止、上限違反、時計停止、時刻wrap、NACK、QUIESCEをWindows ClangとWSL GCC（ASan/UBSan）で検査しています。ホストローダーはSHA不一致でデバイスを開かないことを確認しています。実機への異常注入、抜去・プロセスクラッシュからの復元は未確認です。

次はこのPAF2クライアントを復調器制御へ接続し、新FR版経由で選局・TS受信を確認する段階です。機器内の自走タイマー、DT295用HALと機種固有処理、通常のDriverHost/BonDriver配布物への統合は別途必要です。
