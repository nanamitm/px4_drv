# CECH-ZD1J 統合版の自動検証

対象は n0atl `a360802` からの機種対応、coax `471328e` への更新、TS 世代管理、
設定による受信復帰、アプリ署名を含む Windows 版です。
以下は MSVC v143 / Release-static の x86・x64 と、Go 1.27.3 で得た自動試験の結果です。
USB 実機での測定は含まれません。

| 対象 | 結果 |
| --- | --- |
| `build.ps1` | x86/x64 の全コンポーネントと配布物をビルド成功 |
| 署名付き `build.ps1 -RequireSignedApplications` | 外部 PFX による署名と検証まで成功 |
| TS 同期・カード状態・リング破棄 | 両アーキテクチャで成功 |
| ストリーム停止・パイプ終了・カード待機/排他 | 両アーキテクチャで成功 |
| `stream_epoch_test` | フレーム分割、形式不正、途中切断、新旧 TS 混在防止、ポインター寿命を両アーキテクチャで確認 |
| `torne_foundation_test` | USB 制御、停止競合、初回ロックの絶対期限、復帰間隔と中断を両アーキテクチャで確認 |
| `torne_bootstrap_test` | ジョブ応答・校正・キャッシュ保持/形式不一致・1210 箇所の転送失敗注入を両アーキテクチャで確認 |
| `check_torne_native.py` | Go と C++ の独立した制御トレースが各 9 条件で一致 |
| `torne_codegen/compare_upstream.py` | 固定した元 coax と抽出した FR30 生成器の出力が一致 |
| アプリ署名 | 全 16 配布フォルダーの BonDriver・DriverHost・WinSCard を検証 |
| `application_signature_test.ps1` | 改変、署名除去、ハッシュ書き換え、一覧省略、不正パス、別証明書を拒否。信頼ストアの変更なし |
| WinUSB カタログ | クリーンな Git チェックアウトの全 17 INF、およびコピー後の dist/Driver で署名対象と一致 |
| GitHub Actions 構文 | `actionlint` 成功 |

`tools/package-winusb.ps1` はコミット済みソースと署名済み配布物から ZIP を作成し、
バイナリ ZIP の展開後にアプリ署名と CAT を検証します。
ソース ZIP でも全 INF を署名時と同じ CRLF へ戻し、展開後の CAT と照合します。
配布先の証明書ストア変更や実機操作は、この検証処理の対象外です。

## 実機で残る確認

- CECH-ZD1J の通常受信、弱電界、スキャン速度、TVTest の映像復号
- 複数台の同時受信、空きチューナー選択、別個体のカード共有
- TS 受信と APDU の同時負荷、既存 PX 機との同時利用
- 無入力・ロック喪失からの自動復帰、復帰中の終了とチャンネル変更
- USB/カード抜去・再挿入、EDCB の長時間録画、D/E/S と APDU 最大処理時間

実機確認前の配布設定は `AutoRecovery=false`、`InitialLockRetry=false`、
`OutputErrorPackets=false` です。
移植元の実機測定は各資料の冒頭に由来を記載しており、上記の統合版の確認範囲とは区別できます。
