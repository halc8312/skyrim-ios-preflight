# 開発状況

2026-11-12。ゲームを含まない検証アプリの**iPhoneOS ARM64コンパイル・リンクと、別のARM64 iOS Simulatorでの操作検査が成功**しました。**Skyrimは未起動。インストール可能なIPAは未作成。実機未検証。**

## 確認済み

- Windows 10 Pro 19045 / PowerShell 7.6.5、Xeon E5-1620 v4（4C/8T）、RAM31.9 GiB。初回空きRAM6.2 GiB。F空き87.5 GiB、C空き8.3 GiB。大きな生成やビルドはFへ置き、2並列を上限にする。
- WSL Ubuntu-24.04と `/mnt/f` を利用可能。Python3.12、Git、CMake、Ninja、VS2022 BuildTools、WSL GCC13.3が存在。WindowsにXcode/iPhoneOS SDKはない。
- Steam本体 `C:\Program Files (x86)\Steam` をHKCU/HKLMで確認。libraryfolders登録先はC側と `F:\SteamLibrary`。
- 両登録先に `appmanifest_72850.acf` / `appmanifest_489830.acf` はない。F側SEフォルダーは `Debug.log` のみ。範囲を限定した候補探索で `TESV.exe` / `SkyrimSE.exe` は未検出。購入の有無や、別の未探索場所への保管有無を断定しない。
- 元Unity作業のREADME・開発ガイド・状態を確認し、独立した `F:\SkyrimIOS` に実装。Astraにソース監査、iOS基盤、CIを分担した。
- 固定した上流2ソースを取得・実装と履歴を監査。Skydreamのゲームなし合成検査9件がWSLで成功。詳細は [SOURCE_AUDIT](SOURCE_AUDIT.md)。
- Steam探索／PE同定、元入力と派生入力の分離、誤混入検査、ビルド出自／署名照合、UIKit/Metal検証アプリと疎な4 GiBメモリー検査を実装した。
- Native CをMSVC x64 /W4 /WXとWSL GCCでビルド・実行。4 GiB仮想予約、3ページ12 KiBの使用、境界・32-bit変換・解放を確認。WSL peak RSS 1,576 KiB。iPhoneでの確保可否は未確認。
- [実CI 43071698303](https://github.com/halc8312/skyrim-ios-preflight/actions/runs/43071698303) は終了0。Xcode26.6(17F113)、iPhoneOS26.5(23F81a)、ARM64、最低iOS18.0を確認。Python検査は29件中28件成功、PyYAML未導入による任意のYAML検査1件skip（Windowsで同検査は成功済み）。
- [実CI 43072800930](https://github.com/halc8312/skyrim-ios-preflight/actions/runs/43072800930) でもiPhoneOSビルド・dSYM照合が成功。追加したARM64 iOS26.5 Simulator（iPhone 17 Pro Maxモデル）のUI検査1件が63.911秒、失敗0で成功。起動、左右ドラッグ、保存／読込、Homeから復帰、プロセス再起動後の保存状態復元、実行バイナリーUUID記録、音声のスケジュールを確認。三角形の描画と操作後の位置／回転は保存画像でも確認した。音が聞こえること・Files取込・ログ共有は未検証。
- 同Simulatorで4 GiB仮想予約、16 KiBページ3枚＝48 KiBアクセス、境界確認と解放が `VA PASS`。別のmacOS ARM64ネイティブC検査も16 KiBページで成功。Simulatorの `available=0` は実機の利用可能RAMを示さない。前後footprintは47,765,376→47,863,680 bytesの瞬間値で、ピークやゲームの必要量ではない。
- 成功後、Simulator初回起動が約6分かかった記録を基に起動600秒／UI360秒の上限を設定し、実機用未署名成果物をSimulator開始前にも保持するようにした。終了・SIGINT/SIGTERM・子プロセス停止・無関係プロセス保護・失敗ログ保持を局所検査済み。アプリ本体は成功版と同一で、この保持／期限処理だけの変更ではクラウド検査を繰り返していない。

## 証拠

- `local/environment-20261112-023341.json`: 実行環境・ツールと容量。
- `local/inputs-20261112-023309.json`: Steam照会順、全確認パス、未検出、検査エラー0。exeがないためバージョン／サイズ／ハッシュは未取得であり、空値を既知の入力で埋めていない。
- [PROBE](PROBE.md): C検査・Apple側未検証点・実機手順。
- [CI](CI.md): source検査・ビルド／署名テスト・固定環境・生成物の対応。
- `local/build/native-vs/Testing/Temporary/LastTest.log`: MSVC側の実C検査。
- `local/ci-failures/run-43070949202.log`: macOSの `/var` → `/private/var` 差による初回検査失敗。ルート正規化と範囲外symlink拒否の回帰検査で修正。
- `local/ci-failures/run-43071341237.log` と `local/ci-downloads/run-43071341237.zip`: CoreGraphicsリンク不足の原失敗。明示リンクを追加し修正。原ZIP SHA256 `a794e53c00980e9614f8a0677e37d54de3ccd08b7a58a3b971b13f2b75b3a253`。
- `local/ci-downloads/run-43071698303.zip`: 成功した原ZIPを取得・照合済み、SHA256 `90dafa38b3a13cce50474fc82926255e68a411e8692b1fa25312fade562955e8`。同名の展開フォルダー内 `20261111T174839Z-unsigned-Gcdofr/artifacts/manifest.json` がソースcommit・SDK・バイナリー・dSYM・ログを対応づける。
- 成功時CIソース `c764be0f8fdce122151ac45ab5187abbbc3a15d7`。実行ファイルSHA256 `22da35955b4a517e1c97576b846530be7dc9d293193aaa2f1035a9dd3c7241bc`、dSYMと共通UUID `9E48331D-C9F6-3FD9-8471-5EF2A82FA045`。`installable=false`、未署名.app.zip。署名済みIPAと呼ばない。
- 最新の成功原ZIP `local/ci-downloads/run-43072800930.zip` のSHA256は `b57f2b1b18e31b3b91fe91b2750a77c1b686e964ff89f1a5015dba03e426d21c`。APIのdigestと照合し、内包5成果物・実行バイナリー・dSYMのハッシュも照合済み。ソース `27cafaa7e43b120f9c44030f720d60665bf955de`、iPhoneOS実行ファイルSHA256 `e4f118e0194a78c384257e0eef0a7c0ed32ad5c4e030e5c9b71914ba8257d2cb`、共通UUID `EF1143FE-E8B8-38C3-B77C-EC79590461F5`。展開先 `local/ci-downloads/run-43072800930/ci/20261111T175538Z-unsigned-Vmn7oE/artifacts/manifest.json`。
- Simulatorの原ログ・XCTest結果・画像は同展開先 `simulator/run-20261111T175605Z-2063/`。`scope.json` と `native-test.log`、`attachments/C7450221-BC3B-4DF9-B725-0054E9AD2CEC.txt` がVA結果、`C632D884-07BC-42EC-BCB8-3BA216530064.png` が初期描画、`7A674A0A-708C-4115-9F53-E46620EBC772.png` が再起動後の復元。Simulatorの実行UUID `7AF2E14E-10E0-3A05-A5DF-9775C2C6A88E` はiPhoneOS成果物と別物。最終画面PNGはテスト終了後のホーム画面なので描画の証拠には用いない。
- [非公開Draft PR #1](https://github.com/halc8312/skyrim-ios-preflight/pull/1) に今回の `pc-aot/` とCIを分離。既存mainは変更していない。

## 現在の障害と次の操作

1. **2011年版の入力が未検出。** Steamで当該作品のインストール状態を確認し、ユーザー側でインストール／既存保存先の用意が必要。こちらからSteam設定・元データは変更していない。用意後 `scripts/Find-SkyrimInputs.ps1` を再実行し、展開前後の実ハッシュと生成手順を記録する。
2. **変換の前提が未充足。** capstone/pefile等の上流依存はWindows/WSLにない。未検証のexeハッシュへ固定アドレスを適用しない。承認済みprofileと比較検査を揃えるまで生成は開始しない。
3. **クラウド環境。** Codemagicの利用設定は未確認。ブラウザー操作は現在URLを確実に識別できないという自動安全確認により停止し、以後は既知のGitHub APIでCI結果を取得した。既存の非公開GitHub CIで署名なしコンパイル／リンクと別枠のSimulator検査は完了。新しい有料サービス・課金契約・署名資格情報は追加していない。
4. **実機署名の不足。** iOSバージョン・Developer Modeは回答待ち。今回のbundle IDに合うAd Hoc profile、登録済みUDID、既存distribution証明書が未確認。鍵やtokenをチャットへ貼らない。CIのsecret管理とApple/Codemagic画面で設定する。
5. **ゲーム統合は後続。** D3D9/DXVK/MoltenVK・shader互換、iPhoneOS依存、メモリー順序／浮動小数点、重要API未実装とライセンス由来の課題が残る。タイトル・新規ゲーム・移動／戦闘・本来のセーブ／ロード・継続性能は全て未確認。

次に実機へ渡すべきものは署名・対象端末照合済みの `SkyrimProbe` IPAです。現在そのIPAはありません。準備できた時点で、入れるファイル・操作・返すログを一組で案内します。
