# アプリケーション署名と配布

BonDriver_CECHZD1J `52dd600` の署名・PE ダイジェスト検証を、px4_drv の x86/x64 配布へ適合した実装です。
署名対象は各配布フォルダーの `BonDriver_*.dll`、`DriverHost_PX4.exe`、`WinSCard.dll` です。
アプリ署名には WinUSB の INF/CAT と同じ、上流由来の `px4_drv CA` の署名用証明書を使います。既定の署名付きCIではCATと同じRFC 3161タイムスタンプを付けます。上流で公開されている共通の開発用証明書であり、特定の配布者だけが保有する署名鍵ではありません。

## ビルドと署名

通常の `build.ps1` は署名しない開発ビルドです。上流と同じ証明書で署名する場合は、次のように実行します。

```powershell
.\build.ps1 -UseDriverPublisher -RequireSignedApplications
```

生成済みの配布物へ署名する場合は、Windows PowerShell 5.1で次を実行します。

```powershell
.\tools\sign-applications.ps1 -UseDriverPublisher
.\tests\application_signature_test.ps1
```

`pkg/signing-tools/trustedpub.pfx` と同梱のカタログ署名スクリプトと同じ設定を使います。署名用PFXは配布フォルダーやソースZIPへ含めません。既存のGitHub Actions Secretsは、この既定の署名経路では参照しません。

署名者は `pkg/application-signing/Publisher.cer`、ルートは `Root.cer` に固定しています。

- 署名者のSHA-256: `42D31AA35DD4F1ED3D9FCCDAEB18A29F3BF255AF2004F519B22232C18B93937B`
- ルートのSHA-256: `80DCD1C9D67938EBEF2457229C24E71A82B3121A682E62BF2C98459AF64C365D`

旧版の署名者 `BonDriver CECH-ZD1 J WinUSB` から更新する場合は、DLL/EXEと同時に証明書・検証スクリプト・`application-signatures.json`も新しい配布物へ更新してください。新しいマニフェストは署名者とルートの両方を検証します。証明書の信頼登録だけでは、古いEXEの署名者は変わりません。

外部PFXを指定する従来の引数も残していますが、対応する署名者は上記の証明書です。環境変数またはDPAPIファイルからパスワードを渡し、値をコマンド引数へ直接書く必要はありません。

## 配布物の検証と信頼登録

署名済みフォルダーには `application-signatures.json`、`Publisher.cer`、`Root.cer`、検証スクリプトが含まれます。
`Verify-Applications.cmd` は検証、`Trust-Applications.cmd` は現在のユーザーへの信頼登録、
`Untrust-Applications.cmd` はその解除をダブルクリックで実行する入口です。
次の操作は証明書ストアを変更せず、全ファイルのハッシュ、署名者、CMS 署名、
Windows SIP による PE 本体のダイジェストを確認できます。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Trust-Applications.ps1 -Action Verify
```

自己署名証明書を現在のユーザーで信頼する場合は `-Action Trust`、解除は `-Action Untrust` です。
ルートを `CurrentUser\Root`、署名者を `CurrentUser\TrustedPublisher` へ登録し、Trust は検証成功後に登録へ進みます。
管理者がマシン全体へ適用する場合は `-Scope LocalMachine` を指定できます。
`-WhatIf` は登録・削除予定の確認に利用できます。
証明書はWinUSBドライバーと共通です。特に `-Scope LocalMachine` で信頼を解除すると、同じ証明書を使うドライバーパッケージの信頼にも影響します。旧アプリ用証明書の登録は自動では削除しません。
Smart App Control や組織のアプリ制御ポリシーでは別の許可条件があるため、
自己署名の信頼登録だけで起動が許可されるとは限りません。

## ZIP と GitHub Actions

ソースのコミット後、Windows PowerShell 5.1 で `tools/package-winusb.ps1 -Version <識別名>` を実行すると、
`build/release/<識別名>` へ署名済みバイナリ ZIP、対応ソース ZIP、SHA256SUMS.txt が生成されます。
ZIP 展開後にもアプリ署名と全 INF/CAT を検証します。
Git のアーカイブではチェックアウト時の改行変換が行われないため、ソース ZIP の INF も
署名時の CRLF へ正規化し、展開後のソースに含まれる全 INF/CAT を再検証します。
ソース ZIP は既存の開発用 PFX を含む署名キーを除外しており、カタログとアプリの再署名には外部の署名キーが必要です。

`release_winusb.yml` は `v*` タグと手動実行に対応します。上流の公開開発用証明書を使うため、既定経路の署名にGitHub Actions Secretsは不要です。タグ以外の実行ではGitHub Releaseの公開を伴わず、検証済みZIPがActions artifactに保存されます。PR向けの通常CIは未署名の開発ビルドを生成します。

RTL-SDR 配布では `rtlsdr/rtlsdr_frontend.exe` もアプリ署名とマニフェストの対象です。外部ランタイム DLL は配布元のファイルを維持し、通常ビルドと ZIP 作成前・展開後の配布検査で `runtime-lock.json` の固定ハッシュと照合します。追加する `vcruntime140.dll` は Visual Studio の再頒布用ディレクトリから取得し、Microsoft の署名を検証してハッシュを記録します。配布検査ではこのハッシュと、helper・依存 DLL のインポート先がOS標準DLLまたは同梱DLLであることも確認します。
