# I-O DATA GV-MVP/XZ3 の Windows WinUSB 対応

GV-MVP/XZ3（USB ID `04BB:0530`）の地デジ・BS・110度CSを、Windows標準のWinUSBと
`DriverHost_PX4.exe` で受信できます。選局・TS転送用のファームウェアはDriverHostに内蔵されています。
地デジと衛星で1受信機を共用します。XZ3からのLNB給電は未対応のため、給電が必要なアンテナには外部から給電します。
既定の `LNBPower=0` で使用します。

## インストール

1. 視聴・録画アプリでXZ3を使用している場合は、使用が終わってから作業します。
2. 共通パッケージの証明書を初めて導入する場合は、`Driver` 内の `cert-install.jse` を実行します。
3. 管理者権限のターミナルで、配布フォルダーから次を実行します。

   ```powershell
   pnputil /add-driver ".\Driver\GV-MVP-XZ3.inf" /install
   ```

4. TVTestなどのアプリに合わせて、`BonDriver_XZ3_32bit` または `BonDriver_XZ3_64bit` の内容を配置します。
   既存のpx4_drvと共用する場合は、配布版のDriverHostを使い、既存のINIにGV-MVP/XZ3の機種定義と受信機定義を取り込みます。
   稼働中の共用DriverHostがある場合は、視聴・録画が終わったあとに更新した版を起動します。
5. `BonDriver_XZ3.dll` を選択し、地デジ・BS・CS110をスキャンします。

## Zadigから移行

ZadigのWinUSBが優先されたままの場合は、デバイスマネージャーでハードウェアIDが
`USB\VID_04BB&PID_0530` の機器を選び、「ドライバーの更新」→「コンピューターを参照」→
「コンピューター上の利用可能なドライバーの一覧から選択」→「ディスク使用」で
`GV-MVP-XZ3.inf` を指定します。既存のチャンネル一覧は引き続き利用できます。

専用INFを使う設定では、XZ3の `DeviceInterfaceGUID` は
`{44e46df4-5fbd-40c8-84d6-e56ddceb12be}` です。
Zadigの共通WinUSB GUIDで動作している環境を継続する場合は、XZ3の定義で
`DeviceInterfaceGUID="{dee824ef-729b-4a0e-9c14-b7117d33a817}"` と `Experimental=true` を使います。

## 検証範囲

次の結果は、ZadigでWinUSBを割り当て、共通WinUSB GUIDで列挙した1台のGV-MVP/XZ3で得たものです。

- x64のBonDriverで地デジ・BS・110度CSの選局、無信号チャンネルからの復帰、閉じて開き直した後の受信を確認しました。
  再オープンを含む451,584パケットで同期エラー、TEI、CC不整合は0件でした。
- x86のBonDriverで地デジ・BS・110度CSの受信と再オープン後の受信を確認しました。
  292,864パケットで同期エラー、TEI、CC不整合は0件でした。
- x64版TVTestで地デジ13〜62chのスキャン、受信できるチャンネルの検出、10秒間の録画を確認しました。
- C/Nは選局直後の最初の要求から有効値を返し、無信号では0 dBに戻ることを確認しました。
- 地デジ13・15chを交互に選ぶ `channel_switch_stress` の6周期で、同期エラー、TEI、CC不整合は0件でした。

`GV-MVP-XZ3.inf` と専用GUIDによる導入、DriverHost経由の長時間録画、USBの抜き差し、スリープ復帰、
複数台の同時接続、LNB給電は実機確認の対象外です。
連続受信は診断用の経路で地デジ13chを約15分受信した記録に限られます。

実機試験の詳細はリポジトリの `winusb/experimental/xz3-firmware/README.md` に記録されています。

