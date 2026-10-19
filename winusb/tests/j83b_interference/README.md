# J.83B 干渉の再現試験 (`j83b_interference`)

J.83B を受信中の受信機（録画役）で、同じ USB デバイスや同じ基板の別の受信機（操作役）の Open・選局・Close が、どの条件で CC エラー（ドロップ）を起こすかを調べる実機試験ツールです。録画ソフトの予約の重なりに頼らず、操作の種類と時刻を決めて繰り返し、操作ごとの CC エラーを集計します。

このツールは実機の受信機を占有します。録画予約のない時間帯に、録画ソフトを止めた状態で実行してください。動作中の DriverHost_PX4 と同じ受信機を使うため、録画予約と重なると、録画ソフト側でチューナーを確保できなくなります。

## 構成

| モード | 役割 |
| --- | --- |
| `victim` | 録画役。J.83B を受信し続け、CC エラーを 1 件ずつ、受信量を 100 ms ごとに CSV へ記録します。 |
| `aggressor` | 操作役。1 つのシナリオを繰り返し、各操作の開始・終了時刻を CSV へ記録します。 |
| `analyze` | 2 つの CSV を照合し、各操作の開始から一定時間内の CC エラーを集計します。 |
| `suite` | 録画役を子プロセスで起動し、既定のシナリオを順に実行して集計まで行います。 |

録画役と操作役は別のプロセスで、それぞれ別名の BonDriver を読み込みます。時刻は壁時計のミリ秒で、`DriverHost_PX4.log` の時刻（同じ書式）と照合できます。

## 準備

1. ビルドします。`build.ps1` の対象外のため、個別に MSBuild します。

   ```powershell
   msbuild winusb\tests\j83b_interference.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
   ```

   出力先は `winusb\build\x64\Release\j83b_interference.exe` です（Win32 は `x86`）。

2. 作業用フォルダ（例: `C:\j83btest`）を作り、BonDriver と DriverHost_PX4.exe の配置先（例: `C:\BonDriver`）から、BonDriver を 2 つの名前で置きます。BonDriver は DLL と同じ名前の ini を読むため、名前で役割を分けます。デプロイ済みの DLL へのハードリンクにすると、今後のデプロイでも同じ版になります。

   ```bat
   fsutil hardlink create C:\j83btest\BonDriver_J83B-Victim.dll C:\BonDriver\BonDriver_PX-MLT.dll
   fsutil hardlink create C:\j83btest\BonDriver_J83B-Aggressor.dll C:\BonDriver\BonDriver_PX-MLT.dll
   ```

3. このフォルダの `BonDriver_J83B-Victim.ini` と `BonDriver_J83B-Aggressor.ini` を作業用フォルダへコピーし、使用中の `BonDriver_PX4-C.ChSet.txt`（J.83B 用）と `BonDriver_PX4-T.ChSet.txt` も同じ場所へコピーします。`DriverHostPath` は DriverHost_PX4 が起動していない場合だけ使われるため、DriverHost_PX4.exe の配置先に合わせて書き換えます。

## 受信機の固定

ini の `[ReceiverDefinition0]` の `DeviceGUID` と `Index` で、使う受信機を 1 つに固定します。GUID は `DriverHost_PX4.ini` の `DeviceDefinitionN` と同じ値です。

| 試す関係 | 録画役 | 操作役 |
| --- | --- | --- |
| 同じ USB デバイス（既定） | PX-MLT8PE3 `Index=0` | PX-MLT8PE3 `Index=1` |
| 同じ基板の別 USB デバイス | PX-MLT8PE3 `Index=0` | PX-MLT8PE5 `{7de16cba-9609-432a-83bf-bc14ba785152}` `Index=0` |
| 別の基板 | PX-MLT8PE3 `Index=0` | PX-MLT5PE `{b4eeca78-7c7f-423e-8ab8-305416d1c4f2}` `Index=0` |

チューニング空間の番号は BonDriver の `System` で決まります。録画役（`ISDB-C` のみ）は J.83B が空間 0、操作役（`ISDB-T,ISDB-C`）は地デジが空間 0、J.83B が空間 1 です。チャンネル番号は ChSet の行の順番（0 始まり）です。

## 一括実行 (`suite`)

```bat
j83b_interference.exe suite BonDriver_J83B-Victim.dll BonDriver_J83B-Aggressor.dll <space-c> <victim-ch> <other-ch> <space-t|-1> <t-ch> <repeats> <outdir>
```

例: 操作役の J.83B が空間 1、録画役と同じ周波数がチャンネル 5、別の周波数がチャンネル 12、地デジが空間 0 のチャンネル 3、各 10 回。

```bat
j83b_interference.exe suite BonDriver_J83B-Victim.dll BonDriver_J83B-Aggressor.dll 1 5 12 0 3 10 C:\j83btest\run1
```

`<victim-ch>` は録画役が受信するチャンネルにも使われます。録画役の空間は 0 固定のため、録画役と操作役で同じ ChSet を使い、チャンネル番号を揃えてください。

録画役を起動して 15 秒待った後、次のシナリオを順に実行します。各操作の後に 5 秒保持し、次の繰り返しまで 10 秒空けます。

| シナリオ | 操作 |
| --- | --- |
| `c-open` | Open（電源投入・初期化と TS 出力開始）だけを行い、5 秒後に Close |
| `c-open-tune-same` | Open の直後に録画役と同じ周波数へ選局し、5 秒後に Close |
| `c-open-tune-other` | Open の直後に別の J.83B 周波数へ選局し、5 秒後に Close |
| `c-tune` | Open したまま 2 つの周波数を交互に選局（Open を伴わない選局） |
| `t-open-tune` | Open の直後に地デジへ選局し、5 秒後に Close（`<space-t>` が -1 なら省略） |

結果は `<outdir>` の `victim.csv`、`aggressor.csv`、`summary.txt` です。Ctrl+C で中断すると、録画役も停止して、それまでの記録で集計します。

## 個別実行

```bat
j83b_interference.exe victim <BonDriver.dll> <space> <channel> <seconds|0> <victim.csv>
j83b_interference.exe aggressor <BonDriver.dll> <open|open-tune|tune> <space> <channel> <alt-channel> <repeats> <hold-ms> <gap-ms> <aggressor.csv>
j83b_interference.exe analyze <victim.csv> <aggressor.csv> <window-ms>
```

`victim` に 0 秒を指定すると、Ctrl+C か `suite` の停止要求（名前付きイベント `Local\px4_j83b_interference_stop`）まで記録します。`gap-ms` は集計の窓より長くし、前の操作の影響が次の操作の窓へ入らないようにしてください。

## 集計の読み方

```text
action                           trials failed    hit cc_total cc_max    first_cc_ms
c-open:open                          10      0      7       23      6       820..1410
c-open:close                         10      0      0        0      0              -
```

- `hit` は、操作の開始から窓（`suite` では 3000 ms）以内に CC エラーが 1 件以上あった回数です。
- `first_cc_ms` は、操作の開始から最初の CC エラーまでの時間の最小値と最大値です。
- `baseline` の行は、どの操作の窓にも入らない区間の CC エラーで、同じ長さの窓あたりの期待値を示します。操作の窓の値がこれと同程度なら、その操作との関係は示されません。

CC エラーは録画役が受け取った TS で数えます。復調器の RS 訂正不能とは別の量のため、原因を絞るには、同じ時刻の `DriverHost_PX4.log`（`ExperimentalJ83BBurstMonitor=true` のバースト記録や Open・選局のログ）と照合してください。

## 確認できていないこと

このツールは模擬入力での試験を持たず、実機での実行もまだ行っていません。BonDriver の `OpenTuner` は受信機の Open と TS 出力の開始を、`SetChannel` は選局を行います。Open 単体と TS 出力開始の影響は、このツールでは分けられません。
