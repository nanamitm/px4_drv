# アプリケーション署名と配布

BonDriver_CECHZD1J `52dd600` の署名・PE ダイジェスト検証を、px4_drv の x86/x64 配布へ適合した実装です。
署名対象は各配布フォルダーの `BonDriver_*.dll`、`DriverHost_PX4.exe`、`WinSCard.dll` です。
WinUSB の INF/CAT には既存の PX4 用証明書があり、アプリ署名とは別に管理されます。

## ビルドと署名

通常の `build.ps1` はキーを必要としない開発ビルドです。
署名を含める場合は次のように、外部 PFX とパスワードの取得元を指定してください。
パスワードの値そのものをコマンド引数へ書く必要はありません。

```powershell
.\build.ps1 -SigningPfxPath C:\Signing\publisher.pfx `
    -SigningPasswordEnvironment PX4_APPLICATION_SIGNING_PASSWORD -RequireSignedApplications
```

同一ユーザーが `ConvertFrom-SecureString` で保存した DPAPI パスワードファイルは
`-SigningPasswordFile C:\Signing\password.dpapi` で指定できます。
既存ビルドへ署名する場合は Windows PowerShell 5.1 で
`tools/sign-applications.ps1` を実行する方法もあります。
秘密鍵はメモリ上で開き、ソース、配布ディレクトリ、証明書ストアへ保存する必要はありません。

署名キーは `pkg/application-signing/Publisher.cer` と一致する必要があります。
公開証明書の SHA-256 は
`5B6288396CA6D34EFFE2B1DBFDA3B6EA8B861E08FBFDD7F83356AA1EC594A1EE` です。
証明書を変更するときは固定値と試験も合わせて更新してください。
既存の PX4 開発用 PFX をアプリ署名用の発行者として流用する必要はありません。

## 配布物の検証と信頼登録

署名済みフォルダーには `application-signatures.json`、`Publisher.cer`、検証スクリプトが含まれます。
`Verify-Applications.cmd` は検証、`Trust-Applications.cmd` は現在のユーザーへの信頼登録、
`Untrust-Applications.cmd` はその解除をダブルクリックで実行する入口です。
次の操作は証明書ストアを変更せず、全ファイルのハッシュ、署名者、CMS 署名、
Windows SIP による PE 本体のダイジェストを確認できます。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Trust-Applications.ps1 -Action Verify
```

自己署名証明書を現在のユーザーで信頼する場合は `-Action Trust`、解除は `-Action Untrust` です。
登録先は `CurrentUser\Root` と `CurrentUser\TrustedPublisher` で、Trust は検証成功後に登録へ進みます。
管理者がマシン全体へ適用する場合は `-Scope LocalMachine` を指定できます。
`-WhatIf` は登録・削除予定の確認に利用できます。
信頼登録はアプリ署名用だけを扱うため、WinUSB ドライバーの既存の導入手順も必要です。
Smart App Control や組織のアプリ制御ポリシーでは別の許可条件があるため、
自己署名の信頼登録だけで起動が許可されるとは限りません。

## ZIP と GitHub Actions

ソースのコミット後、Windows PowerShell 5.1 で `tools/package-winusb.ps1 -Version <識別名>` を実行すると、
`build/release/<識別名>` へ署名済みバイナリ ZIP、対応ソース ZIP、SHA256SUMS.txt が生成されます。
ZIP 展開後にもアプリ署名と全 INF/CAT を検証します。
ソース ZIP は既存の開発用 PFX を含む署名キーを除外しており、カタログの再署名には外部の署名キーが必要です。

`release_winusb.yml` は `v*` タグと手動実行に対応し、次の Actions Secrets を必要とします。

- `PX4_APPLICATION_SIGNING_PFX_BASE64`: 固定した公開証明書に対応する PFX の Base64
- `PX4_APPLICATION_SIGNING_PASSWORD`: PFX パスワード

Secrets 不足時は署名済みリリースの作成が失敗します。
PR の開発ビルドは秘密鍵を使用せず、成果物名で未署名と識別できます。
別リポジトリの Secrets は自動で共有されないため、利用先リポジトリでの登録が必要です。
