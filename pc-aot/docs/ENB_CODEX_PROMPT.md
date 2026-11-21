# Codexへ渡すプロンプト（ENB対応の続き）

下の引用の中身を、そのままCodexに渡す。経緯と詳細は [ENB_CODEX_HANDOFF.md](ENB_CODEX_HANDOFF.md)、計画は [ENB_PLAN.md](ENB_PLAN.md)。

> あなたは `halc8312/skyrim-ios-preflight` のENB対応の続きを担当します。作業ブランチは `claude/laughing-cannon-fvpznk` です（`git fetch origin claude/laughing-cannon-fvpznk` で取得し、同じ名前のブランチで作業してください）。最初に `pc-aot/AGENTS.md`、`pc-aot/docs/ENB_PLAN.md`、`pc-aot/docs/ENB_CODEX_HANDOFF.md` を読んでください。
>
> ## 目的
> ENB本体（`d3d9.dll`）は読み込まず、所有者のENBプリセット（`enbseries.ini` と `enbseries/*.fx`）を、アプリ内の後処理で実行します。目標は、ビルドしてすぐiPhone（所有者の iPhone 17 Pro Max）で、有名なプリセットを一通り検証できる状態にすることです。性能の目標は30 fps前後で、品質0〜3の自動調整で守ります。
>
> ## 済んでいること（Linuxで検証済み。作り直さないこと）
> - `ios/runtime-port/enb_ini`、`enb_fx`（解析とロワリング）、`enb_preset`（読み込み、パス計画、品質の自動調整、変数、`IOS_ENB` 診断）。
> - `enb_exec`（D3D9の公開インターフェースでpassを実行）と `enb_runtime`（プリセット全体の実行）。ネイティブDXVK＋ソフトウェアVulkan上で、`experiments/enb-exec` が全画素をCPUの値と比較して17件合格しています（`experiments/enb-exec/README.md` の手順で再現）。
> - `scripts/enb_report.py`（プリセットの一括調査）。MOD機能画面でのENB検出（`mod_features.c`、スイッチはまだ出ない）。
>
> ## 手順
> 1. **所有者のプリセットで確認する。** 所有者が用意した有名なプリセット数個で `python3 scripts/enb_report.py <プリセットのフォルダ>…` を実行してください。その結果で、`enb_fx.c` の `engine[]` と `enb_preset.c` の `effects[]`（ともに一般的な知識による仮の表）と、`enb_runtime.cpp` の変数・段の受け渡しの仮定を実物に合わせてください（仮定の一覧はハンドオフ文書 §6）。直した内容と、対応できない名前・構文の一覧を `docs/ENB_PLAN.md` §5 に記録してください。
> 2. **アプリへ接続する。** ハンドオフ文書 §3 の順に実装してください：`EnbEffectExec::Compile` をホストに静的リンク済みのvkd3d-shaderで実装、`SKYRIM_IOS_ENB`（既定OFF）、起動時の選択・固定（`mod_manager.m` の `FreezeFeatures` と同じ仕組みで `SKYRIM_FEATURE_ENB` を有効化）、DXVKのPresent経路への差し込み（MSAAは解決したテクスチャを渡す）、品質の自動調整（`IOS_PERF` のフレーム時間と端末の発熱を `enb_governor_update` へ）、深度の供給。オフのとき、またはビルドに含めないときは、従来と出力・画素が一致するようにしてください。
> 3. **ゲーム内の値を特定する。** 時刻・天候・室内の値のアドレスと、HUD描画の直前の位置は、固定版生成物から確かめてください。確定できない場合はその項目を止め、診断に「近似」と出し、理由を記録してください。推測のアドレスは使わないでください。
> 4. **試験を足す。** 自作の `.fx` と `.ini` だけで書いてください。Linux（ASan/UBSan）とMac（Apple SDK）で通してください。診断行は数値だけにし、`ios/runtime-port/diagnostic_schema.h` の許可リスト、`tests/native/diagnostic_upload_fixture.json`、関連試験を同時に更新してください。
> 5. **実機で確認する。** ハンドオフ文書 §4 の項目です。同じセーブ・同じ場面で、オフ、品質0〜3、プリセットごとに、フレーム時間・メモリ・発熱を比べ、Windowsで撮った参照画像と見た目を並べてください。結果は `docs/STATUS.md` の先頭に、通った試験の件数、未確認の点、止まっている理由とともに書いてください。
>
> ## 守ること
> - ENB本体、プリセット、`.fx`、画像、ゲームのファイル、生成コード、セーブ、署名情報をGitHubへ入れない。
> - 試験を無効化・スキップ・条件付き除外して通さない。対応できない呼び出しや構文は、成功に見せかけず、名前と理由を日英で表示する。
> - 毎フレームの追加処理には予算を持たせ、超えたら品質を下げる。
> - GitHub ActionsとCodemagicは、所有者の明示的な「go」がない限り実行しない。
> - ルートの `.gitignore` は許可リスト方式。新しいファイルは `git add -f` するか許可の行を足す（`vendor/`、`local/`、`private/` は追加しない）。
> - `207d14e`（DLLのコード変更要求の記録）はENBと無関係です。取り込むか取り消すかは、Codexの判断に任せます。
> - コミットメッセージの最後には、会話で指定された帰属表示の行を付ける。
>
> ## 完了の定義
> ビルドしたIPAで、所有者が選んだプリセットを有効にして起動でき、プリセットごとに、使えた効果、無効にした効果とその理由、30 fps前後を保てた品質が分かること。
