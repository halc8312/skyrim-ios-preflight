# 開発状況

2026-11-12。ゲームを含まない検証アプリの**iPhoneOS ARM64コンパイル・リンクが成功**しました。**Skyrimは未起動。インストール可能なIPAは未作成。実機未検証。**

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
- [非公開Draft PR #1](https://github.com/halc8312/skyrim-ios-preflight/pull/1) に今回の `pc-aot/` とCIを分離。既存mainは変更していない。

## 現在の障害と次の操作

1. **2011年版の入力が未検出。** Steamで当該作品のインストール状態を確認し、ユーザー側でインストール／既存保存先の用意が必要。こちらからSteam設定・元データは変更していない。用意後 `scripts/Find-SkyrimInputs.ps1` を再実行し、展開前後の実ハッシュと生成手順を記録する。
2. **変換の前提が未充足。** capstone/pefile等の上流依存はWindows/WSLにない。未検証のexeハッシュへ固定アドレスを適用しない。承認済みprofileと比較検査を揃えるまで生成は開始しない。
3. **署名／端末。** Codemagicブラウザ接続は操作エラーで確認できなかったが、既存の非公開GitHub CIで署名なしコンパイル／リンクは完了した。新しい有料サービス・課金契約・署名資格情報は追加していない。Simulatorの基本操作検証を別枠で準備中。
4. **実機署名の不足。** iOSバージョン・Developer Modeは回答待ち。今回のbundle IDに合うAd Hoc profile、登録済みUDID、既存distribution証明書が未確認。鍵やtokenをチャットへ貼らない。CIのsecret管理とApple/Codemagic画面で設定する。
5. **ゲーム統合は後続。** D3D9/DXVK/MoltenVK・shader互換、iPhoneOS依存、メモリー順序／浮動小数点、重要API未実装とライセンス由来の課題が残る。タイトル・新規ゲーム・移動／戦闘・本来のセーブ／ロード・継続性能は全て未確認。

次に実機へ渡すべきものは署名・対象端末照合済みの `SkyrimProbe` IPAです。現在そのIPAはありません。準備できた時点で、入れるファイル・操作・返すログを一組で案内します。
