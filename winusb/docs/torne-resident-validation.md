# torne 常駐方式の実機検証

このページは旧配布版の実機試験の記録です。現行配布版では常駐方式と専用INIを廃止しています。

2026-11-05、Windows / WinUSB、CECH-ZD1J (`054c:04b2`) 1台、MSVC v143 Release-static x64で、mAgicVISION常駐方式を地デジ入力に使用しました。BonDriver_PX4 → IPC → DriverHost_PX4 → 常駐コードの経路です。従来のnative torneバックエンドとは機器定義の `ResidentFirmware` で分離されています。

## 再現用設定

`pkg/DriverHost_PX4/DriverHost_TorneResident.ini` を専用ホストの `DriverHost_TorneResident.ini` として配置し、`BonDriver_TorneResident.ini` は同じ名前のBonDriver DLLと対にします。`DriverHost_PX4.exe` を `DriverHost_TorneResident.exe` に改名したホスト、地デジChSet、`firmware/magicvision-torne-resident.bin` も同じフォルダーへ配置します。共通ファームウェアの内容はDT295用の検証済みSBC2と同一です。通常のtorne設定は変更不要です。

`Experimental=true` と `ResidentFirmware=true` の両方が必要です。実USB IDを照合してtorneプロファイルを選び、衛星要求は拒否します。内蔵カードやCNRは未実装のため、録画TSはスクランブル解除前で、signal表示0は受信不良を意味しません。

```powershell
channel_switch_stress.exe .\BonDriver_TorneResident.dll 0 0 4 8 3000
resident_capture.exe .\BonDriver_TorneResident.dll 0 0 10000 .\gr13 2
```

この例のインデックス0/4は同梱ChSetの地デジ13/17chです。受信地域とChSetに合わせて選びます。実機を開くツールのため、利用中のチューナーに重ねて実行しないでください。

## 観測できた点

- native方式が残した5か所のブートパッチを、読み取り専用ダンプで検出。常駐ローダーは `-129` で拒否し、別方式のパッチを強制上書きしませんでした。
- USBを抜き差しした純正状態では、ブート参照7968バイトがファームウェア束の参照と一致しました。
- 実WinUSBでconst OUTバッファに `ERROR_NOACCESS (998)` が発生。書込み可能な一時バッファを完了回収まで保持する修正後、初期化と受信が通りました。この修正はDT295にも共通です。
- 13ch/17ch各5秒の測定で計112640パケット、sync / TEI / CCエラー0。
- 同一BonDriverインスタンスで閉じて開き直し、13chを各10秒・112640パケットずつ取得。2回ともsync / TEI / CC / malformedは0。PAT CRC有効98件、TSID31761、サービス63496/63497/63498/63880を確認しました。
- 13ch/17chを交互に8回、各3秒測定し、計270336パケットで選局失敗・空受信・sync / TEI / CCエラー0。
- 17chの60秒連続保存で687104パケット（129175552バイト）、sync / TEI / CC / malformedは0。PAT CRC有効598件、TSID31760を確認し、13chと異なるストリームへの切替も確認しました。
- 初期化を含む初回選局は約20秒、通常終了の復元は約22秒。設定のパイプ期限はこの待機を許容する必要があります。
- 正常終了後にブートコード7968バイトが純正状態へ一致することを読み戻して確認しました。

測定は最長60秒の連続受信と選局・再オープンを対象とします。長時間録画、弱電界、複数台、カード同時負荷、アクティブ受信中の物理抜去、TVTest画面の映像復号、DT295固有のOUTディスパッチ・RF回路・BS/CSは別の確認が必要です。
