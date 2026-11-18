# 設計判断

## 2026-11-12: PC版2011を独立して開始

- `F:\SkyrimIOS` / `codex/ios-foundation` に作業を分離した。元Unityの `codex/sr01-realism` と未追跡SceneDependencyCacheを変更しない。
- Skydream `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a` を変換／runtimeの土台候補に選定。再生成manifest、埋込みimage、Windows依存の整理、Apple ARM64への既存対応を比較根拠とした。元Android `4cd7686a9a4d801a4176afbdae062a2cbecbb2bb` も保存して比較できるようにした。
- Skydreamでのゲーム描画／継続プレイ、iPhoneOSビルドは未確認。READMEの記述、macOSリンク、smoke test、リリースバイナリーを相互に代用しない。Androidのリリース説明とソース不一致、継承コードのライセンス未解決事項は [SOURCE_AUDIT](SOURCE_AUDIT.md) を参照。
- 購入済み入力を静的変換し、ARM64 AOTへ進む。モデルによる全関数の自由な書直し・全面エンジン再実装・SEやSwitchへの転換は行わない。
- 元Steam exeと展開済みexeを別々に同定する。既知のSHA-1表記が展開前後のどちらか不明なものは検証済みprofileにしない。後に取得した実入力も、版名やprefixだけで互換と判断しない。
- 変換時 `LIFT_MEMORDER=1` / `LIFT_TSO=1`、コンパイル時 `LB_STRICT_ORDER=ON` を正しさ検証の基準案とした。これだけでx86互換とは判断しない。未アラインアクセス、atomic、x87精度、MXCSR／FTZ、flags、呼出規約、32-bitポインターは個別の差分検証が必要。
- 未実装APIの0返却や音声の黙示無音を完了扱いしない。ゲーム統合時に重要な未対応呼出しを呼出元・ゲストアドレスとともに可視化する。今回、ゲームruntimeへ新しいstubは追加していない。

## 2011年版を先行する理由

2011年版はx86・32bit／D3D9、SEはx86-64・64bit／D3D11であり、採用候補の静的変換器は前者を対象とする。既存の変換・Windows API代替・描画経路を再利用できることを優先した。SEではCPU変換器、呼出規約・ポインター、描画の対応範囲を大きく広げる必要がある。[上流の対応説明](https://github.com/ttom795/Skyrim_Android#which-executable) と固定commitの実装を根拠にし、64bitという数だけでiPhoneへの適合性を判断しない。

2011年版の処理も最終的にはARM64アプリへ事前コンパイルする。その中で32bitゲストのアドレスと整数演算の意味を維持する。現在のruntimeが前提とする4 GiB仮想領域の予約と、実際に使う物理メモリー量は別であり、iPhone実機での成立は未確認。SEの広いアドレス空間は将来の大規模MODでは利点になり得るが、今回の初期成功条件を早く達成できる根拠にはしない。

## 静的再コンパイルを主軸にする範囲

CPU側の実行時x86 JITを使わない条件に合わせ、まず既存変換器で元の命令の意味を保ったCを生成し、ARM64へ事前コンパイルする。生成Cにもゲストのレジスター・flags・アドレス処理などの負担は残るため、AOTだけで速度や持続性能を保証しない。分岐先・浮動小数点・メモリー順序・API代替の正しさは個別に検証する。

上流に既に存在するnative置換は、採用前に元の処理との契約比較を行う。今回の文字列・メモリー転送・CRCの限定診断はそのためであり、測定なしの新しい最適化とは区別する。ゲームが動いた後は実機で重い箇所を測り、比較可能な単位ごとにnative化を検討する。GPU側のshader変換と描画互換はCPUのAOTとは別の検証対象とする。

## iOS検証をゲーム統合から分離

英語版取得後の静的展開は、ソースを固定したSteamlessと既存.NET4.8で行った。独立した8試行の出力も保持し、ハッシュを合わせるための任意のバイト修正はしない。全体ハッシュの差だけで実行コードが異なるとも判断しない。固定関数の境界・呼出規約・参照データ・振舞いを確認してからprofileを審査する。版依存フックを使わない汎用discoveryと合成x86比較は別工程として進められるが、ゲーム生成・ARM64実測の代わりにはしない。

- 最初の小規模アプリはApple標準UIKit／Metal／AVFoundationのみ。追加依存を導入せず、iPhoneOSコンパイル・描画・音声・I/O・アプリ状態・VA制約を切り分ける。
- 仮想領域4 GiBを `PROT_NONE` で予約し、先頭・中間・末尾の3ページだけ使用する。4 GiBの物理確保を行わない。Apple entitlementなしを基準とし、失敗時は実測と公式に使える権限を検討する。
- ゲーム描画の候補経路は D3D9 → DXVK → Vulkan/SPIR-V → MoltenVK/SPIRV-Cross → MSL/Metal。Metal三角形の成功はこの互換性の証拠ではない。MoltenVKは調査用commitを固定したが、未ダウンロード・未リンク。
- iPhoneOS用依存はiPhoneOS SDKから生成する。macOS Homebrew版のリンクによる代用はしない。
- CIはXcode 26.6 (17F113)を固定。公式CodemagicとGitHub macos-26イメージの両方で掲載を確認した。iOS最低版18.0とビルドSDK26.5を区別し、実行時に実SDK・Mach-O platform・ARM64・UUIDを検証する。
- 署名なしコンパイルと、既存署名によるAd Hocを別workflowにする。TestFlight/App Store公開、自動trigger、新規有料契約は初期工程に含めない。

## 既存の非公開CIを再利用する範囲

読み取り可能な `halc8312/skyrim-ios-preflight` を発見した。取得時mainは `80f2d73`。既存はXbox由来の別方式と実験コードを含むため、今回のPC版の動作証拠に転用しない。今回のソースは別ブランチの `pc-aot/` に分離し、mainや旧ソース・旧失敗を上書きせず、非公開のコンパイル環境だけを利用する。端末署名にはアプリIDとプロビジョニングの明示的な対応確認が必要。
