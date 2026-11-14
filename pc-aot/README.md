# Skyrim 2011 → iPhone

購入済みWindows版Skyrim 2011のx86コードを再現可能な静的変換でCへ変換し、ARM64へ事前コンパイルしてiPhone 17 Pro Maxで実行する開発です。現在は**PC版build5をiPhoneへ導入して試遊できている段階**です。ユーザーから60 FPSの表示と発熱低減の報告がある一方、瞬間停止・水中表示等は改善中です。未導入候補と実機確認済みの範囲は[STATUS](docs/STATUS.md)で区別し、[段階的なネイティブ化](docs/NATIVE_MIGRATION.md)を進めます。

作業正本は `F:\SkyrimIOS`。Steamへの2011年版インストールはユーザー許可を得て実施しました。取得した日本語版は全ファイルを独立保存済みで、その後の英語への切替にも許可を得ています。既存Unityプロジェクト、元セーブとMODを保持し、入力の加工は独立コピーで行います。ストリーミングや実行時x86 JITを達成手段にせず、SE・SKSE・ネイティブDLL MODは後段に分けます。

## 現在地

[STATUS](docs/STATUS.md) に今回の確認結果・障害・証拠、[DECISIONS](docs/DECISIONS.md) に採用理由、[SOURCE_AUDIT](docs/SOURCE_AUDIT.md) に実装と一次資料の比較を記録します。Skydreamを基盤候補に選び、具体的なcommitは [source-lock.json](source-lock.json) で固定しています。完成したiOS移植版として採用したものではありません。

## 入力探索

Windowsの既存Python 3を使用します。Steamレジストリ → `libraryfolders.vdf` → appmanifest → 実ファイルの順に調べ、管理情報が不足する場合だけFドライブのSteam候補へ範囲を限定します。無関係なファイルの全文検索や元データの変更は行いません。

```powershell
Set-Location F:\SkyrimIOS
./scripts/Find-SkyrimInputs.ps1
# 管理対象外の既知の保存先がある場合だけ明示する
./scripts/Find-SkyrimInputs.ps1 -GameDirectory 'F:\KnownGameDirectory\Skyrim'
```

日時付きの `local/inputs-*.json` に実パス・存在確認・exeのPE・製品／ファイルバージョン・サイズ・SHA-1/SHA-256を保存します。Data全体をハッシュしません。既存manifestを上書きしません。WSLでは `/mnt/f` 等を確認し、Fが見えない状態と検出できない状態を区別します。

元exeと展開済みexeは別入力です。変換の前提と、未検証のハッシュに固定アドレスを適用しない手順は [TRANSLATION](docs/TRANSLATION.md) を参照してください。日本語版exe **1.9.31.0** は対象外として保存しました。続いて英語版 **1.9.32.0** を取得し、上流が示す元exeのSHA-1 prefixと一致することを確認しました。固定したSteamlessをソースからビルドして独立コピーを静的展開しましたが、展開後のハッシュは上流記載と一致せず、互換性は未承認です。[展開手順と比較結果](docs/UNPACKING.md) を保存し、固定アドレスを使うゲームコード生成は保留しています。

## ローカル検証

英語版の汎用解析では90,900関数候補を記録しました。変換器のAF欠落・空jump tableの報告漏れを修正し、負のindexから候補を回収する限定規則も追加しました。文字列4関数の32,000比較に続き、メモリー転送／CRCの5処理を実際の補助処理16個とともに診断用変換し、20,000比較が成功しました。途中の未解決分岐による停止も保存しています。全ゲームの生成やiPhoneでの実行はまだ行っていません。命令比較の範囲は [CPU_VALIDATION](docs/CPU_VALIDATION.md)、購入済み入力を使った限定診断は [TRANSLATION](docs/TRANSLATION.md) に分けて記録しています。

必要な環境は既存Python 3.10以上、Git、CMake、C11コンパイラーです。依存パッケージの自動導入はありません。

```powershell
py -3 -m unittest discover -s tests -p 'test_*.py' -v
cmake -S . -B local/host-build -G 'Visual Studio 17 2022' -A x64
cmake --build local/host-build --config Release --parallel 2
ctest --test-dir local/host-build -C Release --output-on-failure
py -3 scripts/check-public-tree.py
```

Windows/WSLのテストはiPhoneの結果ではありません。ゲスト領域検証は4 GiBの仮想領域を予約し、3ページのみを実体化・アクセスして解放します。

## iOSビルド

`ios/SkyrimProbe.xcodeproj` / scheme `SkyrimProbe` はUIKit・Metal・AVFoundationを使う小規模検証アプリです。ゲームruntimeと外部ライブラリーは含みません。タッチ・描画・音声・Filesからのコピー・検証状態の保存／読込・中断／再開・ログ共有・4 GiB疎メモリーを調べます。検証状態の保存をSkyrimのセーブ成功とは扱いません。

署名なしとAd Hocを分けた `codemagic.yaml` を用意しています。macOS ARM64・Xcode **26.6 (17F113)**・iPhoneOS SDKを要求し、2並列でビルドします。既存の非公開GitHub CIで署名なしiPhoneOS ARM64ビルド・リンクが成功しました。別のARM64 iOS Simulatorでは描画・タッチ・検証状態の保存／再読込・再起動後の復元・中断／再開と、4 GiB仮想予約／48 KiBアクセスが成功しました。原成果物・ログ・画像をローカルへ取得・照合済みです。実機やSkyrimの検証結果ではありません。各ソース版の結果はSTATUSに記録します。

```bash
bash scripts/build-ios.sh unsigned
# 既存の適合する署名設定・登録端末が揃った後のみ
bash scripts/build-ios.sh ad-hoc
```

[CI](docs/CI.md) に設定名・秘密情報の登録場所・成果物とdSYMの対応・実行手順、[PROBE](docs/PROBE.md) に実機操作を記載しています。未署名.appは端末へインストールできるIPAではありません。macOS・Simulator・iPhoneOS・実機の結果を分けて報告します。

## データの境界

`local/`・`private/`・`vendor/`・`generated/`・`build/`・`artifacts/` をGitから除外します。元exe、DLL、ゲームData、ゲーム由来生成C・初期化データ、セーブ、秘密鍵、署名、ビルド成果物は公開ソースや公開CIに含めません。原失敗、archive、IPA、dSYM、入力manifestは削除せず保持します。`check-public-tree.py` は誤混入の検査であり、ライセンス許諾や秘密情報不存在の証明ではありません。
