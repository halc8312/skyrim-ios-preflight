# 設計判断

## 2026-11-12: PC版2011を独立して開始

- `F:\SkyrimIOS` / `codex/ios-foundation` に作業を分離した。元Unityの `codex/sr01-realism` と未追跡SceneDependencyCacheを変更しない。
- Skydream `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a` を変換／runtimeの土台候補に選定。再生成manifest、埋込みimage、Windows依存の整理、Apple ARM64への既存対応を比較根拠とした。元Android `4cd7686a9a4d801a4176afbdae062a2cbecbb2bb` も保存して比較できるようにした。
- Skydreamでのゲーム描画／継続プレイ、iPhoneOSビルドは未確認。READMEの記述、macOSリンク、smoke test、リリースバイナリーを相互に代用しない。Androidのリリース説明とソース不一致、継承コードのライセンス未解決事項は [SOURCE_AUDIT](SOURCE_AUDIT.md) を参照。
- 購入済み入力を静的変換し、ARM64 AOTへ進む。モデルによる全関数の自由な書直し・全面エンジン再実装・SEやSwitchへの転換は行わない。
- 元Steam exeと展開済みexeを別々に同定する。既知のSHA-1表記が展開前後のどちらか不明なものは検証済みprofileにしない。入力がない現段階でハッシュや生成成功を捏造しない。
- 変換時 `LIFT_MEMORDER=1` / `LIFT_TSO=1`、コンパイル時 `LB_STRICT_ORDER=ON` を正しさ検証の基準案とした。これだけでx86互換とは判断しない。未アラインアクセス、atomic、x87精度、MXCSR／FTZ、flags、呼出規約、32-bitポインターは個別の差分検証が必要。
- 未実装APIの0返却や音声の黙示無音を完了扱いしない。ゲーム統合時に重要な未対応呼出しを呼出元・ゲストアドレスとともに可視化する。今回、ゲームruntimeへ新しいstubは追加していない。

## iOS検証をゲーム統合から分離

- 最初の小規模アプリはApple標準UIKit／Metal／AVFoundationのみ。追加依存を導入せず、iPhoneOSコンパイル・描画・音声・I/O・アプリ状態・VA制約を切り分ける。
- 仮想領域4 GiBを `PROT_NONE` で予約し、先頭・中間・末尾の3ページだけ使用する。4 GiBの物理確保を行わない。Apple entitlementなしを基準とし、失敗時は実測と公式に使える権限を検討する。
- ゲーム描画の候補経路は D3D9 → DXVK → Vulkan/SPIR-V → MoltenVK/SPIRV-Cross → MSL/Metal。Metal三角形の成功はこの互換性の証拠ではない。MoltenVKは調査用commitを固定したが、未ダウンロード・未リンク。
- iPhoneOS用依存はiPhoneOS SDKから生成する。macOS Homebrew版のリンクによる代用はしない。
- CIはXcode 26.6 (17F113)を固定。公式CodemagicとGitHub macos-26イメージの両方で掲載を確認した。iOS最低版18.0とビルドSDK26.5を区別し、実行時に実SDK・Mach-O platform・ARM64・UUIDを検証する。
- 署名なしコンパイルと、既存署名によるAd Hocを別workflowにする。TestFlight/App Store公開、自動trigger、新規有料契約は初期工程に含めない。

## 既存の非公開CIを再利用する範囲

読み取り可能な `halc8312/skyrim-ios-preflight` を発見した。取得時mainは `80f2d73`。既存はXbox由来の別方式と実験コードを含むため、今回のPC版の動作証拠に転用しない。今回のソースは別ブランチの `pc-aot/` に分離し、mainや旧ソース・旧失敗を上書きせず、非公開のコンパイル環境だけを利用する。端末署名にはアプリIDとプロビジョニングの明示的な対応確認が必要。
