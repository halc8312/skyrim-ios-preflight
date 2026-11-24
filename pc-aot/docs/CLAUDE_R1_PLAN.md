# MOD第一弾のR1計画（2026年11月22日の作業案）

この文書は当時の引き継ぎです。「通常版23」は計画時の仮番号です。現在の実装と確認状況は [開発状況](STATUS.md) を参照してください。

作成 2026-11-22。起点は `codex/first-wave-apple-check` の `9aee707`。作業ブランチは `claude/gallant-brown-6del0n`（作成時点で `9aee707` と同じ）。

この文書は、**実装担当（Sonnet 5.5、エフォート高または中）が上から順に作業できる**ように、作業ごとに目的、変更先、手順、試験、完了条件、止める条件、コミット単位を定める。計画と見直しは Opus が行う。既存の [第一弾の計画](MOD_FIRST_WAVE_PLAN.md) の原則（§1）と実装時の注意（§11）は、この文書でもそのまま有効。

参照：[開発状況](STATUS.md) · [第一弾の計画](MOD_FIRST_WAVE_PLAN.md) · [SKSEの設計](SKSE_IOS_PLAN.md) · [自分用ビルドの手順](BUILD_YOUR_COPY.md) · [CI](CI.md)

---

## 0. 最終目標と公開条件

**最終目標**：第一弾のMODを含む通常版を、所有者が署名し、本人限定のTailscaleからiPhoneにインストールできる状態にする。

### 0.1 R1の範囲（Opusの判断）

第一弾の24項目を、公開時点の扱いで3段階に分ける。理由：実機での確認は所有者の時間を使うため回数を減らしたい。また、ゲーム本体の新しいアドレス解析が必要な機能（Enhanced Cameraの一人称の体、HighHeelsのNPC対応など）はこの環境では進められない。これらを公開の条件にすると、他のMODの公開まで止まる。

| 段階 | 対象 | 公開条件 |
| --- | --- | --- |
| **必須** | SKSE Classic・SkyUI、PapyrusUtil・Wearable Lanterns・追加セーブ、Fuz Ro D-oh、Precache Killer、Safety Load、データMOD（KS Hairdos、Apachii、XPMSE/XPMS、RRF、CBBE/UNP、BodySlide・FNISの生成済み出力の取り込み）、NiOverrideの重複表示 | §0.2の実機確認がすべて合格 |
| **試験**（既定オフ。プロファイルでオンにする） | RaceMenuの鼻の高さ、ECEの鼻の高さ、HighHeels（プレイヤーのみ） | オンにしてもクラッシュしない、セーブを壊さない、オフに戻すと元どおり。効果が出ない場合は公開してよいが、画面で「試験」と明示する |
| **未対応表示**（機能はオフ固定、検出と説明だけ） | Enhanced Cameraの一人称の体、HDT Physicsの揺れ、USLEEP（DLC 3本がない環境）、任意のDLL・ENB | 検出したことと、未対応の理由を日英で表示する。有効にできない |

試験と未対応表示の機能は、R1公開後の開発（§9）で引き続き進める。

### 0.2 公開の判定（すべて満たしたら署名・配布）

1. **MODなしのプロファイル**：同じコピーしたセーブで、起動、ロード、セーブ、30分の通常プレイでクラッシュがない。起動時間とフレーム時間の中央値が、通常版22から悪化していない（目安は±10%以内。超えた場合は原因を記録して所有者が判断する）。
2. **必須の各MOD**：[第一弾の計画 §8.4](MOD_FIRST_WAVE_PLAN.md#84-実機での比較同じipamod無効のプロファイルとの比較) の観察項目と、T3-1の自動判定がすべて合格。
3. **試験の各MOD**：オン・オフの切り替えと、セーブ・再ロードで壊れない。
4. **CI**：最終コミットで、GitHub Actionsの `first_wave_trial`・`dll_probe`・`mod_screens` を有効にした実行が合格している。
5. **PC試験**：所有者のPCで T2-1 の一括確認がClangとGCC（ASan/UBSan）で合格している。
6. **STATUS.md** に、上記の証跡（実行URL、件数、日付）が記録されている。

---

## 1. 役割分担

| 担当 | できること | できないこと |
| --- | --- | --- |
| **Opus（計画・見直し）** | この計画の更新、各フェーズの終わりのコード見直し（`/code-review`）、公開判定 | — |
| **Sonnet（実装・この環境）** | 公開リポジトリのコード、自作の素材を使う試験、GitHub ActionsのmacOS実行（公開リポジトリの標準ランナーで課金なし。2026-11-22に実行83の課金時間0を確認）、文書 | 所有者の固定版生成物・MOD実物・DLCを使う試験、新しいゲームのアドレスの確定、署名、実機 |
| **所有者のPC（CodexまたはRemote Control経由のClaude）** | 固定版生成物での試験、新しいアドレスの確定、Windows上のLLVMでのゲストのビルド、IPAの組み立てと署名、Tailscaleでの配布 | — |
| **所有者のiPhone** | 実機テスト、診断の送信 | — |

**提案**：所有者のPCをRemote Controlでこのクラウドのセッションに接続すると、Claudeが所有者のPCで固定版生成物の試験を直接実行できる。Codexへの引き継ぎ回数が減る。必須ではない。

---

## 2. Sonnetへの共通ルール

最初に必ず読むもの：`pc-aot/AGENTS.md`、[第一弾の計画](MOD_FIRST_WAVE_PLAN.md) の §1 と §11、この文書の対象作業の節。

1. **ブランチ**：`claude/gallant-brown-6del0n` だけにコミットし、push する（`git push -u origin claude/gallant-brown-6del0n`）。所有者の許可済み。PRは作らない。他のブランチへpushしない。
2. **コミット**：作業ID 1つにつき1つ以上。メッセージは既存と同じく英語の命令文（例：`Accept the HighHeels continue notice in the binding test`）。末尾にこのセッションの帰属行を付ける。試験が通ってからコミットする。
3. **新しいファイル**：ルートの `.gitignore` は「すべて無視して許可したものだけ追跡」の方式。`git add -f` で追加する。`vendor/`、`local/`、`private/` の中は追加しない。
4. **アドレスと配置**：ゲーム本体のアドレス、関数の引数配置、構造体のオフセットは、リポジトリ内の試験かREADMEに根拠があるものだけを使う。**新しく必要になったら、その作業を止めて §8 の「所有者PCへの依頼」に追記する**。推測で埋めない。
5. **試験**：試験の無効化、スキップ、条件付き除外で通さない。既存の環境依存の失敗（[第一弾の計画 §3.3](MOD_FIRST_WAVE_PLAN.md#33-既存の環境依存の失敗変更しない)）は変更しない。C は `-Werror` でClangとGCCの両方で通す。新しいネイティブ試験は `SKYRIM_TEST_SANITIZERS=1` でも通す。
6. **既定オフ**：新しい機能は既定でオフにする。オフのときは従来と同じ動作にする。`SKYRIM_IOS_CLASSIC_SKSE=OFF` の通常ビルドの出力を変えない。
7. **画面の文言**：`mod_manager.m` は `T(英語, 日本語)`、`mobile_ui.m` は `L(英語, 日本語)`。既存の文言と `accessibilityIdentifier` は変えない（シミュレーター試験が照合する）。変える必要がある場合は、同じコミットで `tests/mod-simulator` を更新する。
8. **診断行**：数値だけにする。`ios/runtime-port/diagnostic_schema.h`、`tests/native/diagnostic_upload_fixture.json`、関連試験を同時に更新する。
9. **GitHub Actions**：使ってよい（課金なし）。Codemagicは使わない。同時に走らせる実行は2つまで。同じコミットで、変更なしの再実行を失敗の原因探しのために繰り返さない。1回の実行は約55分かかるため、短い間隔で状態を確認し続けない。
10. **文書**：日本語。STATUS.mdは各フェーズの終わりに、事実（実行URL、件数、未確認の項目）だけを追記する。
11. **止まったとき**：止める条件に当たった、または同じ失敗に2回の修正で対処できない場合は、作業を止める。止まった場所、原因の仮説、試したことを §10 の作業ログに書き、次の作業IDへ進む。

### エフォートの目安

- **高**：並行処理、ゲストのABI、セーブ、状態の遷移が関わるもの（T1-1、T1-3、T2-1、T3-1、T4-1、T4-2）。
- **中**：手順が決まっているもの、文書、CIの実行（T0-1、T1-2、T1-4、T3-2、T4-3、T6-1）。

---

## 3. 優先順位と理由

1. **フェーズ1（ブランチの健全化）**：`9aee707` には既知の回帰（T1-1）がある。通常版22にあってこのブランチにない変更がある可能性もある（T1-3）。土台が壊れたまま先へ進むと、後の確認がすべてやり直しになる。
2. **フェーズ2・3（所有者の確認を少ない回数で済ませる道具）**：公開までの律速は、所有者のPCでの確認と実機テストの往復回数。1回の依頼ですべての確認が終わり、結果を数値の要約で受け取れるようにする。
3. **フェーズ4（R1機能の仕上げ）**：新しいアドレスが不要で、この環境で実装と試験ができるものだけを扱う。HighHeelsの試験起動は環境変数でしか有効にできないため、iPhoneでは選べない。これを最優先で直す。
4. **フェーズ5〜8**：所有者のPC → CI → 実機 → 公開判定 → 配布の順。
5. **R1の後（§9）**：Enhanced Cameraの一人称の体、HighHeelsのNPC、HDTの揺れ、USLEEPの照合。

---

## 4. フェーズ0・1：環境とブランチの健全化

### T0-1 この環境の準備と基準の記録（Sonnet・中）

- 手順：
  1. `git fetch --unshallow origin`（クローンが浅いと、ブランチ間の比較が誤る）。
  2. [第一弾の計画 §3.1](MOD_FIRST_WAVE_PLAN.md#31-web環境の準備) の手順で `vendor/skydream`（固定コミット `ecf814f`）、`scripts/fetch-mod-codecs.sh`、`libarchive-dev`、`libclang-rt-18-dev` を用意する。
  3. `cd pc-aot && python3 -B -m unittest discover -s tests -p 'test_*.py'` を Clang（`CC=clang`）とGCC（`CC=gcc`）で実行する。`experiments/` は、計画 §3.2 の表にある対象を実行する。
- 2026-11-22 の参考値（vendorなし、Clang 18、Python 3.13.18、4コア）：1041件、失敗10、エラー115、スキップ82、約2分。エラーの大半は `vendor/skydream` がないため。
- 完了条件：vendorを用意した後の失敗が、計画 §3.3 の既知の失敗と T1-1 の回帰だけになる。それ以外の失敗は §10 に記録し、原因を調べてから T1-1 へ進む。
- 記録：結果の件数を §10 に書く。コミットは不要。
- 補足：手順が通ったら、Opusに伝える。Opusが、この環境の起動時スクリプトとして保存することを所有者に提案する（承認が必要）。

### T1-1 MODフックの結合試験の回帰を直す（Sonnet・高）

- **現象（確認済み）**：`tests/test_mod_hooks_binding.py` の `test_bind_select_once_and_defaults` が、`compiled=491524` と `524287` の2条件で失敗する。`c4f57a8` では合格する。
  ```
  host: tests/native/test_mod_hooks_binding.c:339: heel_scene: Assertion `!cpu && first == 0x64000 && !second && !third' failed.
  ```
- **原因**：`9aee707` で `ios/private-abi/pc_guest_mod_hooks_library.c` の `skyrim_highheel_scene_guest_notify` が変わった。`skyrim_highheel_transform_guest_selected` が真のとき、`Finish` の後に `highheel_scene.notify(cpu, SkyrimHighHeelSceneContinue, 0, 0, 0)` を呼ぶ。真が返ると、状態を `0` ではなく `SkyrimHighHeelSceneReady` に戻す。試験の偽物の `heel_scene`（339行目付近）は `Arm` と `Finish` しか想定していない。
- 手順：
  1. まず実装の意図を確かめる。`ios/runtime-port/mod_highheel_scene_host.c` が `SkyrimHighHeelSceneContinue` を受けたときの処理と、`9aee707` の STATUS.md の記述（「直前の有効な三人称姿勢」を次の更新でも使う）を読む。実装が意図どおりなら、試験の方を直す。実装に矛盾があれば、直す前に §10 に記録して止める。
  2. `tests/native/test_mod_hooks_binding.c` の `heel_scene` を、`SkyrimHighHeelSceneContinue`（引数はすべて0）も受け付けるようにする。呼ばれた回数を別の `calls[]` に数える。
  3. 160〜176行目付近のゲスト単体の経路で、次の2つを確かめる。
     - 変換の群を選んでいないとき：`Finish` の後の状態が `0`、`Continue` は呼ばれない（従来どおり）。
     - 変換の群を選んだとき：`Continue` が真なら状態が `SkyrimHighHeelSceneReady`、偽なら `0`。
  4. 既存の確認は弱めない。
- 試験：`python3 -B -m unittest -v tests.test_mod_hooks_binding` を Clang、GCC、`SKYRIM_TEST_SANITIZERS=1` で実行する。`experiments/classic-highheels` の `test_highheel_height` も実行する（所有者の入力がない試験はスキップされる）。
- 完了条件：上の試験がすべて合格し、`c4f57a8` で合格していた条件も合格のまま。
- コミット：`Expect the HighHeels continue notice when the transform group is selected`

### T1-2 固定版の試験のコンパイル上限を変更できるようにする（Sonnet・中）

- 背景：`9aee707` の最新の試験起動を確かめる `test_reviewed_app_uses_only_the_temporary_transform_for_a_selected_offset` が、GCCの60秒のコンパイル上限に達した。上限は `experiments/classic-highheels/test_highheel_height.py` の `run_reviewed_fixture` に固定値で書かれている。
- 手順：
  1. `grep -rn "timeout=60" pc-aot/experiments pc-aot/tests` で、固定版の生成物をコンパイルする箇所を一覧にする。
  2. 共通の読み取り関数を作り、環境変数 `SKYRIM_TEST_COMPILE_TIMEOUT`（秒、整数、1〜3600）で上限を変えられるようにする。未設定のときは従来の60秒。範囲外や数字以外の値は例外で止める（黙って既定値に戻さない）。置き場所は `pc-aot/scripts/` の既存の試験用モジュールに合わせる。新しいモジュールにする場合は `test_timeouts.py` などとする。
  3. 実行時の `timeout=10` は変えない。
  4. コンパイルの所要時間が上限の半分を超えたら、警告を標準エラーに出す。
- 試験：共通の関数の単体試験（未設定、正常値、0、3601、文字列）。既存の試験が従来どおりに動くこと。
- コミット：`Let owners raise the reviewed fixture compile limit`

### T1-3 通常版22とこのブランチの差分の確認（Sonnet・高）

- 背景：通常版22（`codex/mod-skse-diagnostic22`、`3209aab`）は、SkyUIの持ち物画面とMCMを実機で確認した版。分岐点は `65f69e8`。22にしかないコミットは18個あり、そのうち9個は同じ件名のコミットがこのブランチにもある。残りの9個は次のとおり。
  - `3209aab` 通常版22のビルド番号、`b4ff309` 21、`624ab01` 20、`d86b6b4` 19（ビルド番号の変更）
  - `a7902c1` Include the Classic save inspector header in the installer SDK check
  - `bc713b5` Include the bounded Classic companion inspector and its native tests
  - `a54128f` Open the installed MOD details by tapping its title in the simulator
  - `a608776` Test installed DLLs in the same simulator as the MOD screens
  - `ca54d01` Check the stable simulator evidence prefix
- 手順：
  1. 同じ件名の9個は `git patch-id` で内容が一致するかを確かめる。一致しないものは差分を読み、このブランチに意図的な変更が入っているかを判断する。
  2. 残りの9個は、変更されたファイルと箇所が `9aee707` に含まれているかを確かめる（`git show <c> --stat` と、対象の箇所の比較）。`a54128f` は `08a47d5`（Tap the visible MOD row in the simulator detail test）で置き換えられた可能性がある。
  3. 結果を表にする：コミット、内容がこのブランチにあるか（あり／一部／なし）、R1に必要か、対応。
  4. 「なし」で、R1に必要なものは `git cherry-pick -x` で取り込む。衝突したら手で解決し、関係する試験を実行する。ビルド番号のコミットは取り込まない（番号は T6-1 で23にする）。
- 完了条件：表をこの文書の §10 に記録する。必要なものの取り込みと試験が終わっている。
- 止める条件：取り込みに、このブランチの意図的な設計変更とぶつかる箇所がある場合。両方の意図を §10 に書いて止める。
- コミット：取り込みごとに `-x` 付きのcherry-pick。表だけの場合は文書のコミット。

### T1-4 GitHub ActionsでApple SDKの確認（Sonnet・中）

- 手順：
  1. ワークフロー `ios-preflight.yml` を、`ref=claude/gallant-brown-6del0n`、入力 `first_wave_trial=true`、`dll_probe=true`、`mod_screens=true` で1回実行する（GitHubのMCPツール `actions_run_trigger`、`method=run_workflow`）。ジョブの条件（2026-11-22確認）：`standard-title-host` は `first_wave_trial`、`dll-probe-sdk` は `dll_probe`、`classic-mod-simulator` は `mod_screens` で動き、3つは並行する。`apple-sdk-preflight`（macOSで `tests/` 全体を実行する）は入力がすべて偽のときだけ動く。前回の合格（実行83）では `standard-title-host` が約54分かかった。
  2. フェーズ1の最後に一度、入力なしの実行もする。macOSで公開の試験全体（`apple-sdk-preflight`）が通ることを確かめる。
  3. 失敗したら `get_job_logs`（`failed_only=true`、`return_content=true`）でログを読み、直してから再実行する。
- 完了条件：`standard-title-host`、`classic-mod-simulator`、`dll-probe-sdk`、`apple-sdk-preflight` が同じコミットで合格。実行URLをSTATUS.mdに記録する。
- 注意：Mac上の `xcrun` の警告やシミュレーターの起動待ちで不安定な場合も、変更なしで同じ実行を繰り返さない。2回失敗したら §10 に記録して止める。

**フェーズ1の終わり**：Opusが差分を見直す。STATUS.mdの先頭に、Claudeの作業開始と、T1-1〜T1-4 の結果を追記する。

---

## 5. フェーズ2・3：所有者の確認を少ない回数で済ませる道具

### T2-1 所有者のPCでの一括確認スクリプト（Sonnet・高）

- 目的：所有者の入力（固定版生成物、MODの実物）が必要な試験を、1回のコマンドですべて実行する。結果は、パスや内容を含まない数値の要約で受け取る。
- 新規：`pc-aot/scripts/first_wave_owner_checks.py`
- 手順：
  1. 目録を作る：`grep -rn "SKYRIM_TEST_OWNED_" pc-aot/tests pc-aot/experiments` と、[第一弾の計画 §8.2](MOD_FIRST_WAVE_PLAN.md#82-固定版生成物での確認) のコマンドから、所有者の入力が必要な試験をすべて列挙する。各試験について、必要な環境変数、実行のしかた（unittestのモジュール、`--generation` を取るスクリプト、実行時のディレクトリ）を表にしてスクリプトに持たせる。
  2. 引数：
     - `--generation <dir>`（`SKYRIM_TEST_OWNED_CLASSIC_LIFT` に渡す）
     - `--inputs <json>`：環境変数名と所有者のファイルの対応。JSONの例を文書に載せる。値はスクリプトの外に出さない。
     - `--compilers clang,gcc`、`--sanitizers`、`--compile-timeout <秒>`（T1-2の環境変数へ渡す）、`--jobs <n>`、`--only <パターン>`
  3. 実行：各試験を独立した子プロセスで動かす。並列数は `--jobs`（既定は CPU数の半分）。
  4. 出力：`pc-aot/local/owner-checks/<UTC時刻>/` に `summary.json` と `summary.md` を書く。内容は試験ID、コンパイラ、結果（pass／fail／error／skip／not-run）、所要秒、失敗時は最後の40行。最後の40行からは絶対パスを `<path>` に置き換える。
  5. **入力がなく実行できない試験は skip ではなく `not-run: missing <環境変数名>` と表示する**。最後に「実行できなかった試験」の数を必ず出す。
  6. 任意の `--guest-build <stage-dir>`：`scripts/build_title_guest.py <stage> --classic-skse --mod-hooks <全群>` を実行し、成否とリンクしたファイル数だけを記録する。全群の名前は `build_title_guest.py` の引数の定義から取る。
- 試験：`pc-aot/tests/test_first_wave_owner_checks.py`。偽の試験モジュール（合格・失敗・時間切れ・入力不足）を一時ディレクトリに作り、要約の内容、パスの除去、`not-run` の扱い、並列実行、`--only` を確かめる。
- 文書：新規 `pc-aot/docs/OWNER_CHECKS.md`（日本語）。Windows（WSL）での準備、コマンドの例、`--inputs` のJSONの例、返送するもの（`summary.md` の中身だけ）。
- コミット：`Run every owner-input check in one bounded pass`

### T3-1 実機の診断から第一弾の合否を判定するスクリプト（Sonnet・高）

- 目的：所有者がiPhoneから送った診断を読み、§0.2 の自動で判定できる項目を合格・不合格・データなしに分ける。目視が必要な項目は一覧にして残す。
- 新規：`pc-aot/scripts/first_wave_device_report.py`
- 手順：
  1. `scripts/receive_private_diagnostics.py` を読み、受け取った診断がどの形式で `private/` に保存されるかを確かめる。その形式を入力にする。
  2. 規則表を作る。各規則は、MOD、診断行の名前、条件、意味で構成する。名前と項目は `ios/runtime-port/diagnostic_schema.h` の正規表現と完全に一致させる。最初の規則の案：

     | MOD | 診断行と合格条件 |
     | --- | --- |
     | SkyUI | `IOS_CLASSIC_SKYUI requested=1 enabled=1`、`IOS_CLASSIC_SKYUI_INPUTS skyui=1 skse_pex=1` |
     | MOD機能 | 有効にした機能ごとに `IOS_MOD_FEATURE ... enabled=1` |
     | PapyrusUtil | `IOS_PAPYRUSUTIL` の `published` が0より大きく、`refused_stale` などの失敗の項目が0 |
     | Wearable Lanterns | `IOS_CLASSIC_LANTERN phase=3 accepted=1` |
     | 追加セーブ | `IOS_CLASSIC_SAVE_STORE` の成功、`IOS_CLASSIC_LOAD_RECORDS matched=1 applied=1` |
     | Fuz | `IOS_FUZ enabled=1`、`substituted` が0より大きく、`errors=0` |
     | ECE | `IOS_ECE_NOSE ... fresh=1 record=1` |
     | HighHeels | `IOS_CLASSIC_HIGHHEEL_ASSETS result=0`、`_PEX phase=1 verified=1`、`_CATALOG result=0`、`_EFFECT start=1 linked=1`、`_SCENE names=31 duplicate=0 limited=0`、試験をオンにした場合は `_TRIAL applied=1` |
     | Safety Load | `IOS_SAFETY_LOAD accepted` が0より大きい |
     | 性能 | `IOS_PERF` の中央値と最大値（`scripts/analyze-frame-timing.py` を再利用）、`IOS_DEVICE_STATE`、`IOS_STAGE` の起動の各段階の時刻 |

     各項目の値の意味は、出力しているコードを読んで確かめる。意味が確定できない項目は条件に使わず、規則に「要確認」と書く。
  3. 2つの報告（MODなしのプロファイルAと、確認するプロファイルB）を受け取り、性能を比較する。
  4. 出力は日本語のMarkdown：自動判定の表、目視確認の一覧（[第一弾の計画 §8.4](MOD_FIRST_WAVE_PLAN.md#84-実機での比較同じipamod無効のプロファイルとの比較) から）、データがなかった項目。
- 試験：`pc-aot/tests/test_first_wave_device_report.py`。合成した診断ファイルで、合格、不合格、データなし、A/Bの比較を確かめる。**規則の診断行の名前がすべて `diagnostic_schema.h` にあることと、規則の例の行がスキーマの正規表現に一致することを確かめる試験**を入れる（スキーマの変更で規則が古くなるのを防ぐ）。
- コミット：`Grade first-wave device diagnostics against the release checks`

### T3-2 R1の実機テスト手順書（Sonnet・中）

- 新規：`pc-aot/docs/DEVICE_TEST_R1.md`（日本語、所有者向け、チェックボックス形式）。
- 内容：
  1. 準備：元のセーブのバックアップ、コピーしたセーブ、診断の受信（`receive_private_diagnostics.py`）の起動。
  2. プロファイルの作り方：A（MODなし）、B1（SkyUI＋PapyrusUtil＋Wearable Lanterns）、B2（Fuz）、B3（髪・骨格・体型）、B4（Precache Killer＋Safety Load）、T（試験：RaceMenu、ECE、HighHeels。1つずつ）。
  3. 各プロファイルでの操作と、診断を送る時点。DLCが不要なものから順に並べる。
  4. 送った後に所有者がすること：T3-1のスクリプトを実行し、出力をClaudeのセッションに貼る。
- コミット：`Write the R1 device test procedure`

**フェーズ2・3の終わり**：Opusが見直す。

---

## 6. フェーズ4：R1機能の仕上げ（新しいアドレスが不要なもの）

### T4-1 HighHeelsの試験をプロファイルの機能スイッチで選べるようにする（Sonnet・高）

- 背景：プレイヤー向けの試験起動は、`mod_manager.m` の690行目付近で `getenv("SKYRIM_CLASSIC_HIGHHEEL_PLAYER_TRIAL")` が `"1"` のときだけ選ばれる。iPhoneのアプリでは利用者が環境変数を設定できないため、実機で試せない。プロファイルの機能スイッチ（`profile[@"features"]`、画面の識別子 `mod-feature-<key>`、`SKYRIM_FEATURE_HDT_HIGHHEELS`）は既にある。
- 手順：
  1. `mod_manager.m` の388行目付近（`profile[@"features"]` を読む箇所）と822行目付近（`launchSetup[@"features"]` に固定する箇所）を読む。Fuz、PapyrusUtilなどがスイッチからフックの選択に至る経路を確かめる。
  2. HighHeelsの試験起動の条件を「起動時に固定した機能に `SKYRIM_FEATURE_HDT_HIGHHEELS` が含まれる」に変える。環境変数は開発用の上書きとして残す（どちらかが真なら選ぶ）。それ以外の条件（`pc_guest_classic_skse_active`、必要なフックの群、実物のESM・PEX・既定INIの照合）は変えない。
  3. スイッチの状態の文（`mod_features.c`）：必要な群がゲストにないときは既存の `GUEST_HOOKS_MISSING` を使う。説明に「試験：プレイヤーのみ。NPCは未対応」を日英で出す（新しい文言として足す）。
  4. 起動時に `IOS_MOD_FEATURE` の行が既存の形式で出ることを確かめる。
- 試験：`tests/test_mod_features.py` とネイティブの試験に、HighHeelsのスイッチの状態の条件を足す。`mod_manager.m` の起動条件は、既存の構文検査（`clang -fsyntax-only`、CIの「Check the installer against the iPhoneOS SDK」）で確かめる。純Cに切り出せる判定は切り出して試験する。シミュレーター試験（`tests/mod-simulator`）にスイッチの表示を足す。
- 完了条件：Linuxの試験とCIのシミュレーター試験が合格する。
- コミット：`Select the HighHeels player trial from the profile feature switch`

### T4-2 ECEのスロットの鼻の高さを、RaceMenuと同じ経路で反映する（Sonnet・高）

- 背景：RaceMenuのプリセットは `profile[@"pendingPreset"]` → 起動時の `PrepareRaceMenuPreset`（`mod_manager.m` の490行目付近）→ ゲストの `CHARGEN_SLIDER` 群の `TakeRaceMenuNoseHeight` → `RaceMenuDidApply` の経路で、標準の鼻の高さに反映される。ECEの `.slot` からの値の読み取りは `skyrim_ece_slot_stock_nose_height()`（`mod_ece_slot.c`。旧形式は1.5倍、−1〜1に制限）として実装と試験が済んでいる。発行済みのスロットは `skyrim_mod_publish_ece_slot()` でDocumentsに置かれる。
- 手順：
  1. `pendingPreset` に種類を足す：`kind = "racemenu"`（未指定は従来どおりRaceMenuとして扱う）または `"ece"`。ECEのときは `package`、`slot`（1〜40）、`sha256`（スロットのファイルの内容）を持つ。
  2. `PrepareRaceMenuPreset` を分ける：種類がECEのときは、そのプロファイルで `SKYRIM_FEATURE_ECE` が有効なこと、発行済みのスロットのファイルのSHA-256が一致すること、`skyrim_ece_slot_stock_nose_height()` が成功することを確かめ、`raceMenuNose` に値を入れる。以降の `TakeRaceMenuNoseHeight` と `RaceMenuDidApply` は共通にする。
  3. 画面：ECEのパッケージの詳細に「このスロットの鼻の高さを適用」を足す。RaceMenuとECEのどちらかを選ぶと、もう一方の保留は置き換わる（文で明示する）。適用後の通知は既存の文を使う。
  4. ECEのTRIを照合する診断（`IOS_ECE_NOSE`）は従来どおり出す。反映の条件には使わない。実機で `fresh=1 record=1` と見た目の変化を確かめる（T3-2の手順書に入れる）。
- 止める条件：反映の前にゲスト側でTRIの照合結果を確かめる必要があると判断した場合や、ゲスト側の変更や新しいアドレスが必要になった場合。§8に依頼を書いて止める。
- 試験：選択の保存と固定、種類ごとの検証（SHA-256の不一致、機能が無効、スロットの読み取りの失敗では反映しない）を、純Cに切り出して試験する（例：`mod_ece_profile.c` に判定関数を足す）。シミュレーター試験にボタンを足す。
- コミット：`Apply an ECE slot's nose height through the stock slider path`

### T4-3 R1の段階を画面に表示する（Sonnet・中）

- 手順：§0.1の「試験」と「未対応表示」の各機能について、`mod_features.c` と `mod_manager.m` の現在の表示を確かめる。足りない説明だけを日英で足す。
  - Enhanced Camera：「カメラの診断のみ。一人称で体を表示する機能は未対応」
  - HDT Physics Extensions：「設定とメッシュの取り込みのみ。髪や衣装の揺れは未対応」
  - USLEEP：DLCが不足している場合の既存の案内を確認する
  - RaceMenu・ECE・HighHeels：「試験」
- 試験：シミュレーター試験に文言の確認を足す。既存の文言と識別子は変えない。
- コミット：`Label first-wave features by their R1 support level`

**フェーズ4の終わり**：Opusが見直す（`/code-review`）。T1-4と同じ入力でCIを実行して合格を確かめる。STATUS.mdを更新する。

---

## 7. フェーズ5〜8：所有者のPC、ビルド、実機、公開

### T5-1 所有者のPCでの確認（1回目）

所有者（またはCodex）に、次をまとめて依頼する。依頼の文はSonnetが作り、所有者に渡す。

1. `claude/gallant-brown-6del0n` を取得する。
2. `python3 scripts/first_wave_owner_checks.py --generation <gen> --inputs <json> --compilers clang,gcc --sanitizers --compile-timeout 600 --guest-build <stage>` を実行する。
3. `summary.md` をClaudeのセッションに貼る。
4. §8の依頼（新しいアドレスなど）があれば、その結果も貼る。

Claudeは失敗を直し、必要なら2回目を依頼する。

### T6-1 公開用のビルド番号と構成（Sonnet・中）

- `ios/runtime-port/Info.plist.in` の `CFBundleVersion` を `23` にする（`9aee707` では18、通常版22は22。iPhoneに22の上から入れるために22より大きくする）。バンドルIDが通常版22と同じかを確かめる。
- `scripts/assemble_title_ipa.py` など、ビルド番号を照合する箇所を確かめる。
- 公開ビルドの構成を `pc-aot/docs/RELEASE_R1.md` に書く：ホストの環境変数（`SKYRIM_IOS_CLASSIC_SKSE=1`、`SKYRIM_IOS_SKSE_DLL_PROBE=1`、`SKYRIM_IOS_MOD_HOOKS=1`、`SKYRIM_IOS_CLASSIC_PAPYRUSUTIL=1`）、ゲストの `--mod-hooks` の群、[自分用ビルドの手順](BUILD_YOUR_COPY.md) の §4〜§6 に沿った組み立て・署名・配布のコマンド（`assemble_title_ipa.py`、`sign_title_ipa.py`、`serve_private_ota.py`）。
- CIを実行し、合格を確かめる（§0.2の4）。
- コミット：`Number the first-wave release candidate as build 23`

### T6-2 所有者による組み立てと署名（所有者）

`RELEASE_R1.md` の手順で、CIの未署名ホストと所有者のゲストからIPAを組み立て、署名し、Tailscaleで配布する。

### T7-1 実機テスト（所有者）と修正の繰り返し

`DEVICE_TEST_R1.md` の手順で確かめ、診断を送る。T3-1の結果をClaudeのセッションに貼る。Claudeは不合格の項目を直し、T5-1〜T6-2を必要な範囲で繰り返す。必須の項目で2回直しても合格しないものは、所有者と相談し、R1の範囲から外すか、公開を待つかを決める。

### T8-1 公開判定と記録（Opus）

§0.2をすべて確かめ、STATUS.mdに公開の記録を書く。署名とTailscaleでの配布は所有者が行う。

---

## 8. 所有者PCへの依頼（作業中に追記する）

新しいアドレス、引数の配置、固定版生成物でしか確かめられない事項が出たら、ここに追記する。書式：作業ID、確かめたいこと、根拠にしたコード、期待する結果の形式。

| 作業ID | 依頼 | 状態 |
| --- | --- | --- |
| T5-1 | `test_reviewed_app_uses_only_the_temporary_transform_for_a_selected_offset` を `SKYRIM_TEST_COMPILE_TIMEOUT=600` でGCC・Clangで再実行 | T1-2の後 |

---

## 9. R1の後（R2以降）

R1を公開した後、次の順で進める。いずれも固定版生成物での解析が必要なため、所有者のPCをRemote Controlで接続するか、Codexとの分担を決めてから始める。

1. **HighHeels**：NPC対応。通常の三人称更新とは別のタスクから骨格を更新する経路の処理。
2. **Enhanced Camera**：一人称で体を表示する処理。PCの調査で確認したゲームコードへの変更要求49件の代替処理を設計する。
3. **RaceMenu・ECE**：鼻の高さ以外のスライダー、ペイント、オーバーレイ。
4. **USLEEP**：公式DLC 3本がそろった環境で変更レコードを照合する。
5. **HDT Physics Extensions**：髪や衣装の揺れ（別の開発として扱う）。
6. **任意のDLL・ENB**：JContainersの調査の続き。

---

## 10. 作業ログ（Sonnetが追記する）

| 日付 | 作業ID | 結果・止まった理由 | コミット・実行URL |
| --- | --- | --- | --- |
| 2026-11-22 | 計画 | Opusが作成。`9aee707` で `test_mod_hooks_binding` の回帰を確認（T1-1）。vendorなしの基準：1041件、失敗10、エラー115、スキップ82 | — |
