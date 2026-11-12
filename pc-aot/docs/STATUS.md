# 開発状況

2026-11-12。**2011版をインストールし、日本語1.9.31.0と英語1.9.32.0を独立保存済み**です。英語版の元exeは上流のSHA-1識別prefixと一致します。独立コピーの静的展開は完了しましたが、展開後のハッシュは上流記載と異なり、互換性は未承認です。ゲームを含まない検証アプリのiPhoneOS ARM64ビルドとSimulator操作検査は成功済みです。**Skyrimは未起動。インストール可能なIPAは未作成。実機未検証。**

## 確認済み

- Windows 10 Pro 19045 / PowerShell 7.6.5、Xeon E5-1620 v4（4C/8T）、RAM31.9 GiB。初回空きRAM6.2 GiB。F空き87.5 GiB、C空き8.3 GiB。大きな生成やビルドはFへ置き、2並列を上限にする。
- WSL Ubuntu-24.04と `/mnt/f` を利用可能。Python3.12、Git、CMake、Ninja、VS2022 BuildTools、WSL GCC13.3が存在。WindowsにXcode/iPhoneOS SDKはない。
- Steam本体 `C:\Program Files (x86)\Steam` をHKCU/HKLMで確認。libraryfolders登録先はC側と `F:\SteamLibrary`。
- 初回探索と15:46の再確認では両登録先に2011版／SEのmanifest・exeがなく、Steamの各Installed値も0だった。F側SEフォルダーは `Debug.log` のみ。その後、ユーザーがSteamインストールを許可し、こちらで2011版の開始画面を開いた。クリック操作の取得エラーのため確定はユーザーが行った。
- 15:58:03 JST、Steamが2011版を `Fully Installed` / `No Error` と報告。`F:\SteamLibrary\steamapps\common\Skyrim`、BuildID15039560、日本語、ダウンロード5,268,473,152 bytesが完了。SEは未導入のまま。
- 日本語版TESV.exeはx86 PE32、17,440,768 bytes、固定値／文字列ともFileVersion・ProductVersion **1.9.31.0**。SHA-1 `68f83f9b866d0abe7f2de2970a8d3e577f4ef7c6`、SHA-256 `c2778d3e80a25618ce731e38612127ec9df4d61d4ce1bfac3c414de87b68aed2`。主要Data9ファイルと音声BSA2ファイルが存在。Data全体のハッシュや起動は実施していない。
- 元exeコピーと全体コピーを独立保存。全体は187ファイル、5,842,262,187 bytes。保存元・コピー先・記録の全パス／サイズが一致し、exeのSHA-256も一致。SteamのSizeOnDisk値とは50 bytes差があるが、実ファイルのコピー比較は一致している。日本語原本・旧未検出記録を上書きしていない。
- 日本語全体コピーを照合後、ユーザー許可を得て英語へ切り替えた（画面操作は取得エラーのためユーザーが選択）。16:23:44 JSTにSteam更新完了、UserConfigとMountedConfig双方がenglish、StateFlags4。英語版TESV.exeは1.9.32.0、x86 PE32、18,024,240 bytes、SHA-1 `f39aecebf6e332616c4c518617073f6e8250e0f8`、SHA-256 `69362f77bc56bf17570a33106d020b6c67fd8f32a294b82f8b687b9cf71efc72`。Android READMEが示す元exeのprefix `f39aeceb` と一致。7セクションで`.bind`あり、フック位置比較は展開まで不可。
- 英語版全181ファイル、6,031,556,590 bytesも独立コピーし、全パス／サイズとexe両ハッシュが保存元に一致。日本語版コピーを保持したまま、Steam側の設定は現在英語。
- 固定したSteamlessソースを既存MSBuild／.NET4.8でビルドし、SteamStub2.1 x86形式の英語コピーを静的展開。ゲームexeは未実行。既定出力は17,462,064 bytes、SHA-1 `17ec1ef6d72781359860f103f600711dfc219182`、SHA-256 `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`。元exe、全体コピー、各試行の元exeハッシュは変わっていない。
- 展開後は6セクション・`.bind`なし・entry VA `0x00f568a1`。上流PE読取処理がimport/TLS構造を受理し、15 DLLから266個の静的importを確認。これはDLLファイル全部が必要という意味ではなく、動的取得・COM・各代替実装の正しさは別検証。実際にimportされる `FlushFileBuffers`・thread priority/affinityには上流の未実装動作があり、統合前の課題となる。
- 上流16フック候補の位置は`.text`内だが、同じ関数やABIである証拠ではなく互換性は未承認。日本語版と英語展開版の16指紋は全て異なる。物理処理4関数・時間変数等も未検証として明記。新規監査の合成検査9件がWindows/WSLで成功。
- 展開オプション8通りと、各出力のセクション終端までの読み取り専用ハッシュを比較したが、上流記載の `67a597df…`／`fed96638…` には一致しなかった。元の署名overlayが保持される一方、出力のsecurity-directory位置がEOF外になる不備も特定。任意のバイト修正で一致させず、元の全出力と失敗を保持。詳細は [UNPACKING](UNPACKING.md)。
- 許可済みのCapstone5.0.9とpefile2024.8.26のみを、SHA-256固定の公式wheelから専用WSL環境へ導入。配布版5.0.9に対してPythonの `__version__` が5.0.7なのは公式ソースの定数更新漏れと照合済み。元ファイルを修正せず、配布メタデータとwheelハッシュで固定する。関数探索の直列／並列一致1件、合成PEの境界・TLS／import・埋込み初期化2件もWSLで成功。
- 未承認の英語展開候補に対し、版依存置換を使わない関数解析をWSL・2並列で実行。16:58:52〜17:05:08 JST、exit0、両JSONの構造と入力の不変を確認。90,900関数候補、1,083 jump table、gap4,029／46,802 bytes。未解決リスト1,201件は重複を除くと1,051箇所。後続監査で漏れていた空jump table2箇所を加え、計1,053箇所を未解決と記録した。完全なコード網羅性・変換成功・profile承認を意味しない。
- 実x86命令と生成Cの比較でAF欠落を再現し、固定パッチを実装。拡張40,632ケース／最適化で、上流はO0/O2各13,016不一致、修正版は各0。算術・flag転送・保持・有限値のx87/SSE比較を検証。原失敗を保持し、独立したAstraレビューも実施。[CPU_VALIDATION](CPU_VALIDATION.md) に範囲・限界を記載。ARM64や全ゲームコードの正しさは未確認。
- 上流16置換＋物理4関数を限定監査し、自己完結した文字列4関数だけを診断用に変換・実行。上流native wrappersとの32,000比較で、戻り値・書込み・保持ABIが一致した。元exeの起動やゲーム全体の生成ではなく、参照側の変換器誤りが共通する可能性は残る。元入力・解析結果・全一時ファイルを保持。空jump tableを未解決として報告する第2パッチも合成検査済み。
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
- `local/inputs-recheck-20261112-154645.json` がインストール前の再確認、`local/inputs-installed-20261112-155938.json` が完了後の入力同定。別記録として保持。
- `private/inputs/steam-72850-japanese-20261112-155938/` に元exe・provenance・discovery、`private/inputs/steam-72850-japanese-full-20261112-160530/` に全体コピーと187ファイルの `complete.json`。ゲーム入力はGit／CIへ送らない。
- `local/inputs-english-20261112-162508.json` が英語版入力同定、`private/inputs/steam-72850-english-20261112-162508/` が全体コピー・discovery・complete記録。`local/pe-audit/english-original-20261112/` は展開前なので `packed_image_not_hook_evidence` / `not_comparable` とした監査。
- `local/pe-audit/japanese-installed-20261112/` と `local/pe-audit/japanese-preserved-20261112/` にPE・セクション・フック候補位置の読み取り専用監査。バイト内容を出力せず、互換性判定は未承認のまま。
- `private/tooling/wheels-20261112-skydream/` に許可済み2wheel、取得ハッシュ、導入ログ、基本動作とCapstone版表示の照合記録。実行環境は `private/tooling/skydream-py312-20261112/`。`local/tests/source-with-approved-deps-20261112/` に追加の上流合成検査3件の原ログ。
- `private/tooling/steamless-cd770bf-20261112/` に固定ソース・成功／失敗ビルド・実行ツールの監査。`private/translation/unpack-steamless-20261112-defaults/` に元コピー・展開候補・recipe・ログ・構造／import監査。`local/pe-audit/english-unpacked-defaults-20261112/` に候補のPE／フック位置指紋。
- `local/translation/english-pair-20261112/translation-plan.json` は実際の元exe／展開候補／recipeを照合し、残るblockerは `unpacked_hash_and_layout_not_reviewed` の1件。exit2、生成は未実行。以前の未検出／依存不足／CRLF失敗記録を上書きしていない。
- `local/translation/english-pair-two-patches-20261112/translation-plan.json` は後続の2パッチも順序・SHA-256を照合済み。残る入力blockerは同じ1件で、ゲーム全体の生成は未実行。
- `private/discovery/basepin-english-1932-20261112-01/result.json` と `discovery.log` に完了した汎用解析。functions JSONのSHA-256 `1fc28ce90d668d635ace8fd641d95720782e24c0f64eaf08cbaf8bf170d15b6d`、gaps JSON `59e370de43a65a031faa97e979892c910a38b3bad4639e72cd6c2c70016e234a`。ソース、元／解析用コピー、依存のハッシュも同記録に固定。
- `local/tests/lifter-flags-20261112T080014.624903Z-58/report.json` にAFの拡張比較。`source-lock.json` は基底commitとパッチSHA-256を固定し、生成contractは順序・実バイトも含む。パッチの内容・順序変更、改行変換、適用先の外側Gitへの混入を検査した。
- `private/analysis/english-contracts-20261112/` に20関数のblock指紋・呼出／参照監査・未解決件数の補正。`private/diagnostics/four-strings-20261112T081458.223724Z-12/` に4関数の限定比較。詳細と再現方法は [TRANSLATION](TRANSLATION.md)。ゲーム由来の生成Cはこの非公開ローカル保管だけに置く。
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
- source公開対象の混入検査と `git diff --check` が成功。Unity側は元の `codex/sr01-realism` と14件の未追跡SceneDependencyCacheを維持。今回の入力・変換器修正ではiOSアプリ本体／native C／SDK build設定を変更しておらず、以前のApple成功結果を今回の新しいCPU診断結果に読み替えていない。

## 現在の障害と次の操作

1. **展開候補の互換性。** 英語1.9.32.0の元exeと展開候補は両ハッシュ・構造・手順を別記録にした。文字列4関数に限定した契約比較は成功したが、全体ハッシュとの差と残る固定関数／globalsは未解決。メモリー転送2箇所の空tableは、`SUB/JB`の分岐で生じる負のindexを解析器が追えていないことまで特定。次は全流入経路を考慮した汎用規則と合成比較を作り、メモリー／CRC等の限定比較へ進む。
2. **変換器の正しさ。** AF欠落を修正し、合成比較は成功。guest callをまたぐflags、x87拡張精度、MXCSR、atomic／ARM64順序などは未解決。固定位置を使うゲーム全体の生成はprofile審査まで行わず、対象関数の限定比較を次に進める。
3. **クラウド環境。** Codemagicの利用設定は未確認。ブラウザー操作は現在URLを確実に識別できないという自動安全確認により停止し、以後は既知のGitHub APIでCI結果を取得した。既存の非公開GitHub CIで署名なしコンパイル／リンクと別枠のSimulator検査は完了。新しい有料サービス・課金契約・署名資格情報は追加していない。
4. **実機署名の不足。** iOSバージョン・Developer Modeは回答待ち。今回のbundle IDに合うAd Hoc profile、登録済みUDID、既存distribution証明書が未確認。既存GitHub repositoryのActions secret／variableはAPIで各0件を確認（組織・environment・Codemagic全体の設定不存在は意味しない）。鍵やtokenをチャットへ貼らない。CIのsecret管理とApple/Codemagic画面で設定する。
5. **ゲーム統合は後続。** D3D9/DXVK/MoltenVK・shader互換、iPhoneOS依存、メモリー順序／浮動小数点、重要API未実装とライセンス由来の課題が残る。タイトル・新規ゲーム・移動／戦闘・本来のセーブ／ロード・継続性能は全て未確認。

次に実機へ渡すべきものは署名・対象端末照合済みの `SkyrimProbe` IPAです。現在そのIPAはありません。準備できた時点で、入れるファイル・操作・返すログを一組で案内します。
