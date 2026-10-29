# RTL-SDR ワンセグ BonDriver の導入

対象は RTL2838UHIDIR（USB ID `0BDA:2838`）です。UHF 13〜52ch のワンセグを受信します。フルセグには対応していません。

## 配布フォルダーの選択

TVTest・EDCBなど、使用するアプリケーションと同じビット数のフォルダーを選んでください。

- 32bitアプリケーション: `BonDriver_RTLSDR_32bit`
- 64bitアプリケーション: `BonDriver_RTLSDR_64bit`

フォルダーには `BonDriver_RTLSDR.dll`、`DriverHost_PX4.exe`、設定・チャンネル定義と、`rtlsdr` サブフォルダーが入っています。`rtlsdr/rtlsdr_frontend.exe` と同じ場所の依存 DLL は受信に必須です。DLLだけを取り出さず、フォルダーの内容をまとめて配置してください。

## ドライバーの登録

1. 対象機器を接続します。
2. 自己署名証明書が未登録の場合は、配布内の `Driver/cert-install.jse` を実行して登録します。
3. 配布フォルダーの `Install-Driver.ps1` を管理者権限のPowerShellで実行します。受信用の Interface 0 に WinUSB ドライバーを登録します。

他のSDRソフトがこの機器を使っている場合は、使用を終了してから進めてください。

## TVTest・EDCBへの配置

1. 使用中のTVTest・EDCB・録画処理を終了し、既存の `DriverHost_PX4.exe` も終了したことを確認します。
2. 選んだ配布フォルダーの内容を、アプリケーションのBonDriver配置先へコピーします。`rtlsdr` サブフォルダーもそのままコピーします。
3. アプリケーションで `BonDriver_RTLSDR.dll` を選び、チャンネルスキャンを実行します。

初期復調には約10秒かかる場合があるため、スキャンの受信待ち時間は25秒など、復調を待てる値に設定してください。TVTestでは、必要に応じて「信号レベルを無視する」を有効にします。既定の非同期選局では要求受付後に復調が進み、データが届くと映像・音声が出ます。

複数台の選択、ゲイン、同期選局のタイムアウト、調査用ログの詳細は、[技術資料・検証記録](../winusb/docs/rtlsdr-bondriver.md)を参照してください。配布フォルダー内では `TECHNICAL.md` に収録しています。

## ソースからビルドする場合

Visual Studio 2022のC++開発ツール・v143・Windows SDK、Python 3、Rust/Cargoを用意します。リポジトリのルートから次を実行してください。

```powershell
rustup target add i686-pc-windows-msvc x86_64-pc-windows-msvc
.\winusb\build.ps1
```

通常ビルドで両ビット数のRTL-SDR配布も生成します。helperとDSPの自動試験、依存DLLの固定ハッシュ、必要ファイル・対応ソースの検査も実行します。配布は `winusb/dist/` に生成されます。helperのCランタイムは静的リンクし、`rtlsdr.dll` が必要とするVisual C++ランタイムは同じフォルダーへ同梱するため、利用者が導入後にRustやVisual C++再頒布ランタイムを追加する必要はありません。
