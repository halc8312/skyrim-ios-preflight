# ENB対応：Codexへの引き継ぎ

作成 2026-11-21。計画は [ENB_PLAN.md](ENB_PLAN.md)。作業ブランチ `claude/laughing-cannon-fvpznk`（`d667103` 以降）。

## 1. いまの状態（正直な範囲）

**iPhoneでENBはまだ効きません。** Web環境で作って試験できる部分（読み取り・計画・制御）は実装済みで、描画に接続する部分（DXVK／Metalでの実行）は未実装です。後者はこの環境にMac、Apple SDK、固定版生成物、実機がないため、Codexの作業です。

| 部分 | 状態 | 場所 |
| --- | --- | --- |
| `enbseries.ini` の読み取り | 実装・試験済み（Clang/GCC、ASan/UBSan） | `ios/runtime-port/enb_ini.{c,h}` |
| `.fx` の構造解析（technique、pass、UI変数と範囲、画像、include、使うENB名、SM4構文の検出） | 実装・試験済み | `enb_fx.{c,h}` |
| プリセットの読み込み、パス計画（品質0〜3）、品質の自動調整、時刻などの変数、`IOS_ENB` 診断 | 実装・試験済み | `enb_preset.{c,h}` |
| MOD機能画面でのENB検出（`enbseries.ini` がDataの直下にあるパッケージ） | 実装。スイッチは出ない（`implemented=0`） | `mod_features.c`、`mod_manager.m`（Mac未確認） |
| 所有者のプリセットを一括で調べる報告ツール | 実装・試験済み | `scripts/enb_report.py` |
| 描画：場面画像の取得、`.fx` のコンパイルと実行、ブルーム・順応・被写界深度、最終出力 | **未実装** | — |
| ゲーム内の値（時刻・天候・室内）の取得 | **未実装**（`EnbGameState` に入れる口だけある） | — |
| 起動時の選択・固定（MOD機能と同じ仕組み）と `SKYRIM_IOS_ENB` のCMakeオプション | **未実装** | — |

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

## 3. 実装すること（描画）

ENB_PLAN.md の §4 の流れを実装する。再利用できるもの：`experiments/d3d9-present-grade`（最終画像への色調処理。描画回数を増やさない）と `experiments/d3d9-postprocess`（Present前の後処理、状態の保存と復元、深度の読み出し）は `codex/first-wave-apple-check` にある。

1. **ビルドの選択**：`SKYRIM_IOS_ENB`（既定OFF）を `ios/runtime-port/CMakeLists.txt` と `scripts/build-title-host.sh` に追加する。オフのときの出力と画素は変えない。
2. **起動時の固定**：`mod_manager.m` の `FreezeFeatures` と同じ仕組みで、プロファイルのENBを選択・固定する（`SKYRIM_FEATURE_ENB` の `implemented` を1にする）。`enb_preset_find` で勝者のDataルートを決め、`enb_preset_load` で読み込む。`IOS_ENB` を出力する。
3. **事前コンパイル**：ゲーム開始前に、`usable` な `.fx` をvkd3d-shaderでSM3へコンパイルする（ホストに静的リンク済み）。D3DXのエフェクト機能（`ID3DXEffect`）は未実装なので、`enb_fx` の解析結果（technique、pass、変数、サンプラー）から、pass ごとに頂点・ピクセルシェーダーを作るか、`.fx` を pass ごとのHLSLに分けて渡す層を作る。コンパイルできないものはその効果だけ無効にして理由を表示する。
4. **変数の供給**：`.fx` のUI変数（`ui_name`）を `enbseries.ini` の値で埋める（`enb_value_parse`）。ENB固有の変数は `enb_vars_compute` の値で埋める。時刻・天候・室内は、固定版生成物で値のアドレスを特定して `EnbGameState` に渡す（特定できない間は固定値か端末の時刻で近似し、その旨を診断に出す）。
5. **実行**：present-gradeと同じ位置（DXVKのPresent）で、`enb_plan_build` の計画どおりに実行する。HUDより前に掛けるため、HUD描画の直前の位置を固定版生成物から特定する（特定するまではHUD込みの画像に掛ける）。
6. **品質の自動調整**：`IOS_PERF` のフレーム時間と端末の発熱状態を `enb_governor_update` に渡し、返された品質で計画を作り直す（シェーダーのコンパイルは品質を変えても再利用する）。目標フレーム時間は約33.3 ms（30 fps）。
7. **深度**：深度を使う効果は `needs_depth` のときだけ解決する（RESZ／INTZ）。実機のMetalで使えるか確認する。

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
> 2. `SKYRIM_IOS_ENB`（既定OFF）を追加し、起動時の選択・固定、事前コンパイル、変数の供給、実行（present-gradeの位置）、品質の自動調整を、ハンドオフ文書の §3 の順に実装してください。オフのときの出力と画素は従来と一致させてください。
> 3. 時刻・天候・室内の値とHUD直前の位置は固定版生成物から特定し、特定できない場合は止めて理由を記録してください。推測のアドレスは使わないでください。
> 4. 診断行は数値だけにし、許可リストと試験を同時に更新してください。ENB本体、プリセット、`.fx`、ゲームのファイル、生成コードはGitHubへ入れないでください。
> 5. 自作の `.fx` と `.ini` で試験を書き、Linux（ASan/UBSan）とMac（Apple SDK）で通してください。実機での確認項目はハンドオフ文書の §4 です。GitHub ActionsとCodemagicは所有者の「go」がない限り実行しないでください。
>
> 目標は、ビルドしてすぐiPhoneで有名なプリセットを検証できる状態です。性能の目標は30 fps前後です。

## 7. 所有者に必要なもの

- 検証するENBプリセット一式（複数）と、対応するENB本体の版。Windowsで同じ場面を撮った参照画像。
- 固定版生成物、Mac（Xcode、iPhoneOS SDK）、iPhone 17 Pro Max。
