# 開発状況

2026-11-13。**PC版build1は実機で元処理90,390関数をロードして実行し、PresentとBethesda動画のデコードまで到達しました。本人は音声と入力反応を確認しましたが、映像は左上に縮小され、ほぼ黒く、ロード途中で進まなくなりました。** 新PCアプリIDは`org.skyrimios.pc2011`、表示名は`SkyrimIOS`。既存Xbox版とは別アプリです。所有ゲームデータ26ファイル／5,909,061,966 bytesは読み戻しSHA256照合済み。**正しいタイトル描画とゲームプレイは未確認です。**

## 2026-11-13 build2 描画・画面処理の修正

- 実機2実行のログを`private/iphone-display-20261113-012319/`へ回収。論理956×440に対しMetal drawableは2868×1320。ゼロ余白時にPresentの出力矩形をnullへ変える処理が、DXVKに論理サイズを選ばせていた。通常のPresentには原点0でも実ピクセル矩形を渡すよう修正。
- 黒画面のログには`shadowSmplr`未宣言によるMetalシェーダー／パイプライン生成失敗がある。[MoltenVK #2768](https://github.com/KhronosGroup/MoltenVK/issues/2768)と一致。固定SPIRV-Crossのcombined-image aliasで既存texture参照に対応するsampler参照も生成する修正を追加。ゲーム由来シェーダーの文字列置換は行わない。
- 自作SPIR-Vの正順alias・逆順aliasで、実際の固定コンパイラーの修正前欠落／修正後定義を確認。別bindingの対照は出力不変。Windows側の実コンパイラー比較結果は`local/ios-render-repro/fixtures/`。同じ3件をiPhoneOS Metalコンパイラーで検証する工程を次回の一括ビルドに含める。
- ロード時は別スレッドのPresentが止まり、mainはSleepを繰り返していた。UIKitのrunloop処理をmainのSleep／PeekMessageでも行い、workerの入力処理はSDLキューの取り出しだけにする。drawable照会もmainで更新した値をworkerへ渡す。Present内部の停止位置は未確定なので5段階の進捗とUI pump時刻を記録する。
- descriptor pool枯渇警告は動的割当へ進んでおり、RAM不足や停止原因とは断定しない。この挙動を隠す変更はしていない。build2のMacビルド・署名・実機確認はまだ未完了。

- [6回目Codemagic](https://codemagic.io/app/6ad977fcf240708df6941076/build/6af5ee029679c85ac0af2a68)／公開cloud commit `ec032691f4ac07f233f4acc113d342ce9f259e52` は約29分のMac割当待ち後、4分01秒で停止。修正版MoltenVKのARM64/iPhoneOSビルドは成功したが、Macで動かす自作検証ツールにworkflowの`IPHONEOS_DEPLOYMENT_TARGET`が漏れ、iOS対象としてリンクされて起動時にsignal 9になった。原ログは`local/codemagic-title6/`。検証ツールのconfigure/buildだけiOS環境を外し、macOS SDK／targetを明示して、実行前にMach-O platform MACOSを確認する修正を追加。Metalコンパイラーの実検証は未完了。

## 2026-11-12 夜の実機タイトル到達作業

- `codex/ios-title-milestone`で、固定アドレスのnative置換を使わず元のguest処理を残す診断経路を実装中。未承認の入力profileを互換性承認済みにはしていない。
- 第5パッチで関数間のCPU flags保持を修正。小さな実リフター／native x86比較8ケースは、両入口方式とも修正前失敗・修正後成功。
- 所有英語入力から90,390関数／512分割のC生成が448秒で完了。510候補は変換拒否として記録。元入力は不変。514 translation unitsのARM64コンパイルと未解決symbol禁止のリンクが成功。`private/title-20261112-01/arm64-ios/result.json`に記録。未署名frameworkは150,660,464 bytes、SHA256 `721bf7684148623c0cfc6c676935c3123eaf14925128e300489744228dc69665`。まだゲームは実行していない。
- 公開iOS runtime、SDL2＋DXVK＋MoltenVK等の依存、private frameworkとのcallback ABIを用意。ゲームコードと入力はPC内で作り、クラウドは公開ホストだけをSDKビルドする。
- 最小確認は公開ソース混入検査、overlayアンカー、共有CPUヘッダー、ABIアダプタのARM64コンパイル。private frameworkのARM64/iPhoneOS18.0構造、署名用余白、署名前内容のbindingを既存ローカル検証器で確認。Mac本ビルド、署名、実機タイトル描画は未完了。
- Codemagicの既存認証・無料枠でPC版のmanual workflowを追加。初回 `6af59d9e9679c85ac0af08de` は2分50秒、vkd3d 2.0の `--without-metal` によるObjective-C条件の未初期化で失敗。原ログは`local/codemagic-title1/`に保持。公式configureの分岐を確認し、実iPhoneOS SDKのMetal/Objective-C検出を有効にする修正だけを加えた。
- 2回目 `6af5a1789679c85ac0af0a5b`／公開cloud commit `7d65f484dd4399ed8b55a299b63659af977a5b3f` は5分17秒。SDL/FFmpeg/vkd3d/MoltenVKのビルド後、`lipo -verify_arch`の引数順で停止。原ログは`local/codemagic-title2/`。引数を入力ファイル→`-verify_arch arm64`へ修正し、アプリコンパイルは独立したエラーも一度に採取するよう`make -k`を指定。
- 3回目 `6af5a56f9679c85ac0af0bdb`／公開cloud commit `eca3e393aedcc3d30a38ea9a0dd38aaabcef034b` は4分42秒。全依存のビルド・ARM64確認後、SDK外のSDL2 prefixをCMakeが再root化し、package発見に失敗。実インストール先をログで確認してexact config path＋`NO_CMAKE_FIND_ROOT_PATH`へ修正。同じroot-only検索の最小再現は修正前exit1、修正後exit0。原ログは`local/codemagic-title3/`。ゲーム由来C・入力・署名鍵は送信していない。

- 4回目 `6af5a9a19679c85ac0af0d5d`／公開cloud commit `d51d761a1bb55388681e3e22d77ed2906017a688` は6分01秒。SDL2検索・全DXVKコンパイルは通過。runtimeを98%まで進めて独立エラーを採取し、失敗はdesktop debugの`LB_BISECT`撮影処理によるiOS禁止`system()`の1件のみ。iOS overlayでは同機能を要求した時だけ明示エラーへ変更し、通常描画を維持。原ログは`local/codemagic-title4/`。直前の無料枠は使用100/500分、subscription未有効。

- [5回目Codemagic](https://codemagic.io/app/6ad977fcf240708df6941076/build/6af5adb89679c85ac0af0f0c)／公開cloud commit `6d6bf479fbb2ba4cb21d07ba4d26e42da842a18f` は8分21秒で成功。Xcode26.6、SDK26.5、ARM64/iPhoneOS/minOS18.0。全依存、DXVK、runtime、Objective-C入口、callback adapterをコンパイル・リンク。public host ZIP SHA256 `11f930ed9c59b36f98afbd9681912d77f88f20b7d3c4cbafb4e127f27ffe7f74`、host Mach-O12,194,488 bytes／SHA256 `8707a2d789ae749b8b4ccf1654f943782a23e5c3be2a27db2da9c2717926d097`。原成果物は`local/codemagic-title5/`。
- SDK側`x86.h`と`PcGuestABI.h`はローカルprivate側とLF正規化後に一致。`rt_table.h`の全文一致検査は失敗したが、差は既にレビューしたhost側の一度だけ代入する`g_modules`宣言のconst解除だけと確認。FnEntry/FnModuleの定義は同じで、private frameworkのstrict-link import集合に`g_modules`はない。追加のprivate再コンパイルは不要。
- `private/title-delivery-20261112/SkyrimIOS-PC2011-build1-unsigned.ipa`をローカル生成。SHA256 `c9c3331c72904d978339048582a2090d9389c2534fe7b758363fc4bffb70973d`、4ファイル。全ZIP CRC、2つのARM64/iPhoneOS18.0イメージの未署名構造・署名余白・terminal LINKEDIT・app identityを既存署名器の検証で確認。署名鍵は開いていない。Appleログイン画面とUSB未接続を確認して本人操作待ち。

## 2026-11-13 実機導入

- 本人ログインと発行直前の承認を受け、PC版専用App IDとDevelopment profileを作成。既存証明書1枚と登録済みiPhone1台だけを対象に、Extended Virtual Addressing／Increased Memory Limit／Increased Debugging Memory Limitを付与。Appleの署名チェーン、CMSと内部DERの一致、証明書・端末・App ID・有効期限・権限の完全一致をPC内で検証。
- 既存の保護済み鍵をPC内で使い、2つのARM64イメージ・39,761コードページ・4リソースの署名と再構成を検証。署名済みIPAは`private/title-signing-20261112/SkyrimIOS-PC2011-build1-signed.ipa`、SHA256 `7ea6f152a3bfc68ec7b08481263623e1680555e5eb2f8572b8cb45c3721333c2`。原IPAとゲーム入力は保持。秘密鍵・ゲーム由来コードはクラウドへ送信していない。
- 既存USB信頼だけで新PCアプリをInstallし、100%終了とインストール後のBundle ID・build・Team IDを確認。Xbox版へのUpgradeや削除は行っていない。アプリを開く前に所有データを転送し、全26ファイル／5,909,061,966 bytesの読み戻しSHA256照合が成功。入力PEも期待SHA256に一致。本人に「SkyrimIOS」の初回起動と表示状況を依頼済み。

## 確認済み

- 残る固定置換7関数とmotion4関数を静的監査した。物理schedulerは11 blocks／50命令で呼出しがなく、次の限定比較対象にできる。他のmotion4関数は195 blocks／1,616命令、109呼出し（動的29、TLS import4を含む）があり、オブジェクト状態・呼出先の比較は未完了。5箇所のclock読取は上流の期待数に一致したが、動作の一致を意味しない。
- 上流のSKSE無効時に、CMakeがruntimeのsource一覧を置き換えてF3DAudioを落とす問題を再現し、第4パッチで保持するよう修正。元のcommand区間を使うCMake検査2件・5設定で確認した。合成の空sourceを用いたtarget属性の検査であり、音声処理の実行やiPhoneOSリンクではない。誤混入検査はこのCMakeファイルだけを追加許可し、関連13検査がWindowsで成功。
- 負のindexから候補を回収する第3パッチを実装。12合成検査と独立した20＋8＋1検査、実入力の2関数限定解析を通過。分岐先候補8件は既知blockと一致し、元のtables／rangesは維持する。全流入経路の完全性は証明できないため未解決の表示は残す。閉じたPE mmapを参照した原失敗も保存し、入力バイトを保持する方法へ修正した。
- メモリー転送2処理／CRC3処理を、元の補助処理16個を含む計21関数だけ診断用に変換・実行。WSLで20,000比較が成功し、終了0、最終1,639件の入力／ソース照合に不一致なし。元exeを起動した結果でも、ARM64／実機の結果でもない。最初の7関数診断は未解決targetで停止し、原失敗を保持。追加した14着地点は算術flagsを読まないことも独立確認した。
- その停止を調べた結果、非空tableでも負のindex側を見落とす2箇所が判明。既存の1,053箇所に加え、**既知の未解決は計1,055 unique sites**。元の全体解析JSONを上書きした数ではなく、限定監査による補足。第3パッチはこの別のCMP/JB/NEG規則までは解決していない。
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

- `private/analysis/remaining-overrides-20261112-01/audit.json` が7置換のABI／呼出し／global監査（SHA-256 `ba4805deb9b2396fcb357c1834883da37c32ba02c4685ae690315c0f5e3e8a97`）。`remaining-physics-20261112-01/findings.json` が4 motion関数の結果（SHA-256 `62898c29d3107d0d8cd6c7343d6333d1646e3e6b03d220bc26fdb061df1bc074`）。新たなゲーム由来コードの生成・実行はしていない。
- `local/tests/desktop-audio-source-20261112/baseline-evidence/` に音声source欠落の原失敗とCMake属性／ログを保存。第4パッチSHA-256は `24e8e69260880dc5c5fd9b6d6ca35e405c9cde5ad3898ea59695a9b15254cee4`。既存の文字列・primitive成功結果は変更しておらず、診断時の固定パッチ構成もそのまま保持する。
- `local/translation/english-pair-four-patches-20261112/translation-plan.json` で4パッチの実ハッシュが一致。残るblockerは互換profile未承認、`executed:false`。ゲーム全体の生成や追加のAppleビルドはしていない。
- `private/analysis/negative-table-real-probe-20261112/` が閉じたmmap参照の原失敗、`negative-table-real-probe-v2-20261112/` が修正後の2関数解析と12検査ログ。独立検査は `local/tests/negative-roots-independent-*`。第3パッチSHA-256は `21f610a0af715992a6b16b2adcacd0924e4d020cc7c7cd6c88beabc9338668e3`。
- `private/diagnostics/five-primitives-reviewed-20261112-01/` が未解決target `F53AE4` の原失敗、`five-primitives-reviewed-20261112-02/report.json` が20,000比較の成功と1,639照合。生成CのSHA-256は `50f5f3c2e900cab5704a846085d5466be14b662c3f82fef2b5f8075eead7aa7e`、保存したrunnerは `53ac73449d05b2bdce994ad138f287f100be513a375237a36150f65f3500c99e`。全生成物・コンパイラー中間物・stderr/stdoutを非公開で保持。
- `private/analysis/primitive-contracts-20261112-04/audit.json` が21関数の入力／block／呼出・table監査（SHA-256 `7d9be136d7f3a89f1d42b3d693f66631a5154234680807e8cf618c7e873feafd`）。旧01〜03も保存。`backward-memmove-cfg-20261112/` にNEG分岐の原因、算術flagsの限定確認、誤った途中説明を訂正する `flags-correction.json`、1,055箇所の補足記録。詳細と再現手順は [TRANSLATION](TRANSLATION.md)。
- `local/translation/english-pair-three-patches-v2-20261112/translation-plan.json` は最終3パッチを照合し、exit2、残るblockerは互換profile未承認だけ。プリフライト26検査、primitive wrapper9検査も成功。iOSアプリ・ビルド設定は今回変更せず、クラウド再ビルドは行っていない。
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

1. **署名・インストール・データ転送は完了。** 上記5回目の成功成果物を再利用し、追加Macビルドは行っていない。ゲーム側のprivate frameworkを含むIPAはPC内だけに置いている。
2. **実機初回起動。** 全データの転送・読み戻し照合まで完了し、本人にPC版「SkyrimIOS」を1回開いて表示状況を報告するよう依頼した。結果とPC内で回収するアプリログを確認する。既存Xbox版のアプリ・セーブ・署名済みIPAは保持。
3. **実行時の正しさ。** 元のguest処理を残す経路で90,390関数のARM64生成／リンクは完了したが、ゲーム未実行。入力は互換承認済みではなく、510変換拒否候補、既知の動的呼出し先、Win32代替API、x87／SSE、ARM64メモリー順序、D3D9互換には実行時の未知が残る。固定アドレスの未検証置換は使用しない。
4. **到達条件。** 実機で所有データを読み、ゲーム自身の動くタイトル／メニューを確認する。firstPresentやprobe・Simulatorの成功だけでは達成としない。新規ゲーム、セーブ／ロード、操作性、音声、性能はさらに後続。

クラウドへ送るのは公開ホストだけ。PC内のprivate frameworkと組み合わせて署名し、別アプリとしてiPhoneへ渡す。署名済みIPA・実機インストール・全データの転送照合は完了。実機タイトルの確認を進めている。
