# ENB対応：Codexへの引き継ぎ

作成 2026-11-21。計画は [ENB_PLAN.md](ENB_PLAN.md)。作業ブランチ `claude/laughing-cannon-fvpznk`（`d667103` 以降）。

## 1. いまの状態

**iPhoneでENBはまだ効きません**（アプリへの接続が未完了）。ただし、`.fx` を実際に実行して画素を検証するところまでは、Linux（ネイティブDXVK＋ソフトウェアVulkan＋vkd3d-shader 2.0）で実装・検証済みです。

| 部分 | 状態 | 場所 |
| --- | --- | --- |
| `enbseries.ini` の読み取り、`.fx` の構造解析、プリセットの読み込み、パス計画、品質の自動調整、変数、`IOS_ENB` 診断、MOD機能画面でのENB検出、一括報告 | 実装・試験済み（Clang/GCC、ASan/UBSan） | `enb_ini`、`enb_fx`、`enb_preset`、`scripts/enb_report.py` |
| `.fx` のロワリング（technique・pass・サンプラー・`RenderTarget` 状態・テクスチャ宣言を取り出し、vkd3d-shaderに渡せるHLSLにする） | 実装・試験済み | `enb_fx.c` の `enb_fx_lower` |
| passの実行（D3D9の公開インターフェース経由）：シェーダー作成、定数表（CTAB）からの定数・サンプラーの結び付け、HLSLの初期値、複数pass、縮小した中間画像、名前付き `RenderTarget<N>`、状態の保存と復元、シーン内での呼び出し拒否 | 実装。**ネイティブDXVK＋lavapipe上で、全画素をCPUの値と比較して合格** | `enb_exec.{h,cpp}`、`experiments/enb-exec` |
| プリセット全体の実行：ini値のUI変数への反映、`#include` の展開、ENB変数、TextureBloom/Lens/Adaptationの受け渡し、品質ごとの縮小、省いた段の中立画像、効果単位の無効化 | 実装。同上のデバイスで合格（17件） | `enb_runtime.{h,cpp}` |
| 起動時の選択・固定、`SKYRIM_IOS_ENB`、アプリのPresent経路への接続、ゲームの時刻・天候・室内の取得、HUD直前の位置 | **未実装**（Apple SDK・固定版生成物が必要） | — |
| アプリ側のシェーダーコンパイル関数（`EnbEffectExec::Compile`） | 未実装。vkd3d-shaderの呼び出しは `enb_exec_test.cpp` の `compileHlsl` と `d3dx_shader.cpp` が手本 | — |
| 深度（TextureDepth）の供給、ブルーム・順応・レンズの**内部の計算**（ENB本体が行う部分） | 未実装。現状は、プリセットの `enbbloom.fx` などが書く内容をそのまま実行するだけ。ENB本体固有の処理は再現していない | — |

検証の範囲：Linuxのソフトウェア描画での画素の計算と状態の扱い。Apple GPUの速度、MoltenVKの出力、実際のプリセットは未確認です。

## 2. 最初にやること（所有者のプリセットで）

所有者の有名なプリセット数個を、それぞれ「`enbseries.ini` と `enbseries/` を含むフォルダ」として用意し、次を実行する。

```sh
cd pc-aot
python3 scripts/enb_report.py /path/to/preset-a /path/to/preset-b ...
```

出力で次を確認し、`enb_fx.c` の `engine[]` 表（ENBが渡す名前の一覧。**一般的な知識による仮の表で未検証**）と `enb_preset.c` の `effects[]` 表（`enbseries.ini` の `[EFFECT]` のスイッチ名。同じく**未検証**）を実物に合わせて直す。

- `unsupported name:` に出た名前（供給できないENB固有の変数や画像）。
- `SM4 syntax:` が出たファイル（DX9のSM3専用なので使えない）。
- `include: … (MISSING)` と `image resource … (MISSING)`（プリセットの欠品。大文字小文字は区別しない照合）。
- `[EFFECT]` のスイッチ名が実物の `enbseries.ini` と合っているか（合っていないと「効果がオンのまま」になる）。
- 画像（`enbpalette.png` など）の形式（PNGのデコードに使える部品があるか）。

この結果を `docs/ENB_PLAN.md` の §5 の表に反映する。

## 3. 実装すること（アプリへの接続）

再現手順は `experiments/enb-exec/README.md`。再利用できるもの：`enb_runtime`（プリセット全体）、`enb_exec`（pass実行）、`experiments/d3d9-present-grade`（最終画像への色調処理。`codex/first-wave-apple-check`）。

1. **コンパイル関数**：`EnbEffectExec::Compile` を、ホストに静的リンク済みのvkd3d-shaderで実装する（`vkd3d_shader_compile`、HLSL→`VKD3D_SHADER_TARGET_D3D_BYTECODE`、`hlsl.profile` と `entry_point` を指定。`enb_exec_test.cpp` の `compileHlsl` と同じ）。
2. **ビルドの選択**：`SKYRIM_IOS_ENB`（既定OFF）を `CMakeLists.txt` と `scripts/build-title-host.sh` に追加し、`enb_exec.cpp`、`enb_runtime.cpp` をホストへ加える。オフのときの出力と画素は変えない。
3. **起動時の固定**：`mod_manager.m` の `FreezeFeatures` と同じ仕組みでプロファイルのENBを選択・固定する（`SKYRIM_FEATURE_ENB` の `implemented` を1に）。`enb_preset_find` で勝者のDataルートを決める。`IOS_ENB` を出力する。
4. **実行位置**：DXVKのPresent経路（present-gradeと同じ位置）で、フレームを読めるテクスチャへ複製（MSAAは解決）して `EnbRuntime::run` を呼ぶ。HUD直前の位置は固定版生成物から特定する（特定するまではHUD込みの画像に掛ける）。`EnbRuntime::release` はデバイスのリセット前に呼ぶ。
5. **ゲームの状態**：時刻・天候・室内を固定版生成物から特定して `EnbRuntime::setGameState` に渡す（特定できない間は固定値か端末の時刻で近似し、診断で示す）。
6. **品質の自動調整**：`IOS_PERF` のフレーム時間と端末の発熱を `enb_governor_update` に渡し、返された品質を `run` に渡す。目標フレーム時間は約33.3 ms。
7. **深度**：深度を使う効果のため、`setDepth` に読めるテクスチャを渡す（RESZ／INTZ、確認済みの手順は `d3d9-postprocess` を参照）。
8. **ENB本体固有の処理**：所有者のプリセットが前提とする、ブルーム・順応・レンズ・被写界深度の本体側の計算が必要かを、`enb_report.py` と実機の見た目で判断する。

## 4. 実機での確認項目

同じセーブ・同じ場面で、オフ、品質0〜3、プリセットごとに比べる。

- フレーム時間（`scripts/analyze-frame-timing.py`）、メモリ、発熱（目標：30 fps前後）。
- 見た目：Windowsで同じプリセットを入れた参照画像と並べる（完全一致は目標にしない）。
- オフのとき従来と画素が一致すること。
- 品質が自動で下がり、戻るときに振動しないこと。
- 起動時の事前コンパイルの所要時間と、ゲーム中の引っかかりがないこと。
- UI（HUD）への影響（HUD込みで掛かる場合の見え方）。

## 5. 止まる条件

- 時刻・天候・室内の値、HUD直前の位置が固定版生成物から確定しないとき：その項目を止め、診断に「近似」と出して、理由を記録する。
- 所有者のプリセットが使う名前や構文が、vkd3d-shaderのSM3で扱えないとき：その効果を無効にして一覧に出す（成功に見せかけない）。

## 6. Codexへのプロンプト（コピーして使う）

> `pc-aot/docs/ENB_PLAN.md` と `pc-aot/docs/ENB_CODEX_HANDOFF.md` を読み、ブランチ `claude/laughing-cannon-fvpznk` からENB対応の続きを実装してください。ENB本体（`d3d9.dll`）は読み込まず、所有者のプリセット（`enbseries.ini` と `enbseries/*.fx`）をアプリ内の後処理で実行します。
>
> 1. まず所有者のプリセット数個で `python3 scripts/enb_report.py <プリセットのフォルダ>…` を実行し、結果で `enb_fx.c` の `engine[]` 表と `enb_preset.c` の `effects[]` 表（ともに未検証）を直してください。直した内容と対応できない名前の一覧を `docs/ENB_PLAN.md` §5 に記録してください。
> 2. `enb_runtime` と `enb_exec`（Linuxで画素検証済み）をアプリへ接続してください。`SKYRIM_IOS_ENB`（既定OFF）、vkd3d-shaderのコンパイル関数、起動時の選択・固定、Presentへの差し込み、品質の自動調整を、ハンドオフ文書の §3 の順に実装してください。オフのときの出力と画素は従来と一致させてください。
> 3. 時刻・天候・室内の値とHUD直前の位置は固定版生成物から特定し、特定できない場合は止めて理由を記録してください。推測のアドレスは使わないでください。
> 4. 診断行は数値だけにし、許可リストと試験を同時に更新してください。ENB本体、プリセット、`.fx`、ゲームのファイル、生成コードはGitHubへ入れないでください。
> 5. 自作の `.fx` と `.ini` で試験を書き、Linux（ASan/UBSan）とMac（Apple SDK）で通してください。実機での確認項目はハンドオフ文書の §4 です。GitHub ActionsとCodemagicは所有者の「go」がない限り実行しないでください。
>
> 目標は、ビルドしてすぐiPhoneで有名なプリセットを検証できる状態です。性能の目標は30 fps前後です。

## 7. 所有者に必要なもの

- 検証するENBプリセット一式（複数）と、対応するENB本体の版。Windowsで同じ場面を撮った参照画像。
- 固定版生成物、Mac（Xcode、iPhoneOS SDK）、iPhone 17 Pro Max。
