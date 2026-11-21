# MOD第一弾 実装計画（Skyrim 2011 LE 1.9.32・通常版）

作成 2026-11-21。基準は `codex/mod-first-wave` の `c23cf1e`、作業ブランチは `claude/skyrim-ios-mod-first-wave-17hieg`（作成時点で同じコミット）。

この計画は、実装担当（低いエフォートまたは別モデル）が上から順に作業できるように、項目ごとに変更先、実装内容、試験、完了条件、コミット単位を定める。Web環境で確かめられない部分は「Codex引き継ぎ」に分ける。固定版の生成物から確定していないアドレス、引数配置、ABIは推測で埋めず、その項目を止めて理由を記録する。

参照：[SKSEの設計とFirst-wave gameplay checks](SKSE_IOS_PLAN.md) · [開発状況](STATUS.md) · [MOD管理の設計](MOD_MANAGEMENT_DESIGN.md) · [DLL互換の試作](../experiments/skse-plugin-vm/README.md) · [Fuzの試作](../experiments/fuz-voice/README.md)

## 1. 原則（全項目共通）

1. 元のゲーム、DLC、MODアーカイブ、セーブは読み取り専用。書き込み先は、対象プロファイルの展開ビュー、アプリ専用の設定ディレクトリ、利用者が明示的にコピーしたセーブに限る。
2. すべての新機能は既定でオフ。オフのとき、またはビルドに含めないときは従来の動作とする。`SKYRIM_IOS_CLASSIC_SKSE=OFF` の通常ビルドの出力を変えない。
3. 毎フレームの処理は追加しない。どうしても必要な場合は、理由、対象スレッド、呼び出し頻度、オフ時の負担（分岐1回など）と、測定方法（§8.4）を記録する。
4. 対応できない呼び出しや入力は、成功に見せかけずに、原因が分かる結果コードと日英の表示を返す。
5. 画面の文言は既存の日英切り替えに合わせる。`mod_manager.m` は `T(英語, 日本語)`、`mobile_ui.m` は `L(英語, 日本語)` を使う。
6. 送信用の診断行は数値だけにし、`ios/runtime-port/diagnostic_schema.h` の許可リスト、`tests/native/diagnostic_upload_fixture.json`、関連試験を同時に更新する。パス、ファイル名、プロファイル名は送らない。
7. ゲーム本編、MOD本体、生成済みゲームコード、セーブ、署名情報をGitHubへ入れない。試験用のDLL、ESP、PEX、NIF、HKX、BSA、音声は試験の実行時に生成する（`scripts/check-public-tree.py` が拡張子で拒否する）。
8. 試験を無効化、スキップ、条件付き除外して通さない。既存の環境依存の失敗（§3.3）は記録だけ行い、変更しない。
9. GitHub ActionsとCodemagicは、所有者の明示的な「go」がない限り実行しない（2026-11-14の指示）。Apple SDKの確認はCodex側に引き継ぐ。
10. Classic v1のゲストABI（`PcGuestSkseCallbacks` の4項目）は変更しない。追加のフックは別のABIで足す（§4.2）。

## 2. 現状の棚卸し（c23cf1e時点）

| 項目 | 既存の実装・試作 | 既存の試験 | 未接続・残り |
| --- | --- | --- | --- |
| SKSE Classic 1.7.3・SkyUI 5.1 | プロファイルごとの「SkyUI 5.1 の試験」（`skyuiTrial`）。Classic v1ゲスト結合、Skydreamの部分的なSKSE（INI、MOD一覧、追加の入力関数の改変を含む）。`IOS_CLASSIC_SKYUI`、`IOS_CLASSIC_VM`、`IOS_CLASSIC_RELEASE`、`IOS_CLASSIC_HOOK` の診断行。SKSEの7zで `Data/` を優先する取り込み修正 | `tests/test_classic_*.py`（26件）、`tests/mod-simulator`（Mac） | 取り込み時と起動時にSKSEスクリプトの不足を知らせない。MCM再読込の回帰を固定する試験がない。画面上端の見切れの原因が未特定 |
| Fuz Ro D-oh 6.1 | `experiments/fuz-voice`：音声選択の方針、音声パス代入の差し込み、会話メニューと周囲のNPC会話の字幕、ゲームのリソース検索、各段階化スクリプト | 自作4件と固定版生成物4件（Webでは後者をスキップ） | 段階化した単位はホスト関数を直接呼ぶ。通常版のゲスト（別のdylib）からホストへ渡る経路、MODの検出、設定、有効化がない |
| PapyrusUtil 3.3・Wearable Lanterns 4.0.2・追加セーブ | x86インタープリターとWindows互換、PEXの型照合（395件中394件一致）、`classic_settings`（JsonUtilの7メソッド）、`classic_papyrus_values.c.in`（型付き関数の公開）、起動試験（`SKYRIM_IOS_CLASSIC_SETTINGS_PROBE`）、`skse-profile-json`、ModEvent・操作イベントの試作、追加セーブの保存庫・コピー・確認、保存公開・新規ロードの試作 | `experiments/skse-plugin-vm`（107件、17件スキップ）、`tests/test_skse_*.py`、固定版生成物を使う各実験 | VMの世代管理がない。ゲームプレイ中の関数公開がない。ModEventと操作イベントは未登録。`.skse` の保存と復元はアプリに未接続 |
| RaceMenu 3.4.5・ECE 1.59・NiOverride 3.4.4 | `.jslot` を `CharGen/Presets` へ配置、ECEの1〜40番の `.slot` をDocumentsへ配置 | `test_mod_archive.py` のプリセットとスロットの試験 | 外見へ反映する経路がない。ECEの形式が未解析。NiOverrideの重複を表示しない |
| ゲームデータ・骨格・生成済み素材 | データルート判定、公式DLCの選択、FOMOD、64ビットHKXの警告、生成済み素材の取り込み | `test_mod_archive.py`、`test_mod_plugins.py` | USLEEPと3つのDLCを使う依存の試験、骨格の勝者表示、FNIS出力の欠品確認、生成元プロファイルとの対応付けがない |
| 髪・カメラ・ロード・物理 | `classic-chargen-precache`（初回の先読みを切り替える段階化）、`classic-camera`（カメラ状態の読み取り）、HDT向けXML・INI・メッシュの配置 | 各実験（固定版生成物の部分はスキップ） | ゲストへの接続がない。DLLのコード変更要求を一覧にする診断がない。Enhanced Camera、Safety Load、HDTのDLL本体は未対応 |

## 3. 環境とベースライン（P0）

### 3.1 Web環境の準備

`pc-aot` で次を実行する（どれも無視対象のディレクトリに入る）。

```sh
cd pc-aot
mkdir -p vendor && git init -q vendor/skydream
git -C vendor/skydream remote add origin https://github.com/meizfl/skydream.git
git -C vendor/skydream fetch --depth=1 origin ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a
git -C vendor/skydream checkout -q --detach ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a
bash scripts/fetch-mod-codecs.sh
apt-get update && apt-get install -y libarchive-dev libclang-rt-18-dev   # ASanとlibarchiveの試験用
export SKYRIM_SKYDREAM_SOURCE=$PWD/vendor/skydream SKYDREAM_SOURCE_DIR=$PWD/vendor/skydream CC=clang
```

### 3.2 2026-11-21のWeb環境での結果（Clang 18、Python 3.13）

| 対象 | 結果 |
| --- | --- |
| `tests/test_mod_*.py` | 36件合格。`SKYRIM_TEST_SANITIZERS=1`（ASan/UBSan）でも36件合格 |
| `experiments/fuz-voice` | 8件中4件合格、固定版生成物の4件はスキップ。ASan/UBSanでも同じ |
| `tests/test_classic_*.py` | 26件合格、5件スキップ（Macと所有者の入力が必要） |
| `experiments/skse-plugin-vm` | 107件合格、17件スキップ |
| `tests/test_diagnostic_upload.py`・`test_private_diagnostics.py`・`test_build_title_guest.py`・`test_title_assembly.py`・`test_game_language.py` | 合格（Mac専用はスキップ） |
| `tests/test_skse_*.py` | 99件中、下記の4件以外は合格 |

### 3.3 既存の環境依存の失敗（変更しない）

- `test_skse_cosave_opened_load`、`test_skse_cosave_snapshot` の3件：このPythonの `fcntl` に `F_ADD_SEALS` がない。
- `test_skse_cosave_generation.GenerationTests.test_concurrent_writers_and_busy_retry`：SQLiteの競合時に期待した `BUSY` にならない。
- GCC 13で `experiments/skse-cosave/mod_history.c` の `slot` に `-Werror=maybe-uninitialized` が出る。Clangでは出ない。

### P0-1 GCC 13の警告を修正

- 変更：`experiments/skse-cosave/mod_history.c` の `skse_mod_resolve_form` と `skse_mod_resolve_handle` で `uint8_t slot = 0;` とする。動作は変わらない（未解決時は `*out` を書かない）。
- 試験：`CC=gcc python3 -B -m unittest discover -s tests -p 'test_classic_interpreted_cosave.py'` と、Clangでの同じ試験。
- コミット：`Initialize the resolved MOD slot for GCC 13`

## 4. 横断設計

### 4.1 X1 プロファイル単位のMOD機能（検出・選択・診断）

**目的**：取り込んだ配布アーカイブの中身から第一弾のMODを認識し、プロファイルごとに既定オフの機能として選べるようにする。利用者にDLLごとの変換作業を求めない。

- 新規 `ios/runtime-port/mod_features.{c,h}`（純C、ホストで試験可能）。
  - 機能ID：`SKYRIM_FEATURE_SKYUI`、`_FUZ`、`_PAPYRUSUTIL`、`_RACEMENU_PRESET`、`_ECE_PRESET`、`_PRECACHE_KILLER`、`_ENHANCED_CAMERA`、`_SAFETY_LOAD`、`_HDT_PE`、`_HDT_HIGHHEELS`。
  - 静的な表に、表示名（日英）、必須ファイル（Data相対、大文字小文字を無視）、認識するDLL、必要なゲストのフック群（§4.2）、ビルド条件、根拠の欄を持たせる。
  - DLLの認識は、ファイルパス、大きさ、SHA-256、PEのバージョンリソース、DLL診断の `Query` が返す名前と版で行う。**名前と版の期待値は、所有者のアーカイブで既存の「DLL互換性」画面を開いて得た値だけを表に入れる**（§8.1）。値が未確定の機能は「未確認」と表示し、有効化できないようにする。
  - PEのバージョンリソースは新規の `skse_pe_version_info()`（`experiments/skse-plugin-vm/pe_image.c` に追加、読み取りだけ）で取得する。
  - 入力は、展開順のパッケージのDataルートと元のDataルートのリスト。出力は機能ごとの状態（`ABSENT`、`PRESENT`、`UNRECOGNIZED_VERSION`、`DUPLICATE`、`GUEST_HOOKS_MISSING`、`BUILD_EXCLUDED`、`AVAILABLE`）と、重複したDLLの提供元。
- 取り込み時：`package.json` に `dlls`（Data相対パス、大きさ、SHA-256、PE版、`Query` の名前と版、結果コード）を記録する。`Query` の実行はDLL診断ビルドだけで行い、取り込みキュー上で既存の作業上限を使う。
- プロファイル：`profile[@"features"] = {id: @YES}`。既存の `skyuiTrial` はキーを変えずにSkyUIの選択として扱う。`RestoreLastGoodSetup` と `launchSetup` に `features` を含め、起動時に固定する。保存に失敗したら `skyuiTrialToggled` と同様にスイッチを戻して通知する。
- 画面：設定 → MOD → 詳細 に「第一弾MODの機能」を追加し、検出した機能だけをスイッチと状態の文で表示する。既存の `mod-skyui-trial` の識別子は変えない。新しいスイッチの識別子は `mod-feature-<id>`。
- 診断行：`IOS_MOD_FEATURE id=<0-15> requested=[01] available=[01] enabled=[01] reason=<0-15>`。
- 試験：`tests/native/test_mod_features.c` と `tests/test_mod_features.py`。PEのバージョンリソースは試験で生成する。不明な版、二重のDLL、大小文字の別名、シンボリックリンク、上限を超える大きさ、プロファイル切り替え（別のルート集合では別の結果）、展開順で後のパッケージが勝つこと。
- コミット：`Detect first-wave MOD features per profile`

### 4.2 X2 ゲスト側のMODフック（別のABI）

**目的**：段階化した生成コードの単位（例：Fuzの音声パス代入）から、アプリ本体の処理を呼べるようにする。通常版のゲストは別のdylibで、システム以外の未定義シンボルを持てない（`build_title_guest.py` の `SYSTEM_SYMBOLS`）。そのため関数表を起動時に結合する。

- 新規 `ios/private-abi/PcGuestModHooksABI.h`。
  - `PC_GUEST_MOD_HOOKS_ABI 1u`、シンボル `skyrim_guest_bind_mod_hooks_v1`。
  - フック群のビット：`FUZ_MENU=1`、`FUZ_WORLD=2`、`VM_LIFECYCLE=4`、`CHARGEN_PRECACHE=8`、`CAMERA_OBSERVE=16`、`CHARGEN_SLIDER=32`、`SAVE_LIFECYCLE=64`。ゲストはコンパイルした群を返し、ホストは使う群を選ぶ。
  - 表は `version`、`bytes`、群ごとの関数ポインタ。関数の型は既存の試作のヘッダーをそのまま使う（`lift_menu_seam.h`、`lift_world_seam.h`、`lift_voice_seam.h`、`voice_fallback.h`）。構造体の値返しは両側を同じClangでarm64向けにコンパイルするため使ってよい。
- 新規 `ios/private-abi/pc_guest_mod_hooks_library.c`（ゲスト側）。
  - 段階化した単位が呼ぶ名前（`skyrim_fuz_host_forget_response`、`skyrim_fuz_host_choose_menu_voice`、`skyrim_fuz_host_note_menu_assignment`、`skyrim_fuz_host_capture_menu`、`skyrim_fuz_host_force_menu`、`skyrim_fuz_host_capture_world`、`skyrim_fuz_host_admit_world`、`skyrim_fuz_host_world_after_copy`、`skyrim_fuz_host_release_chatter_title`、`skyrim_fuz_host_world_general`、以降の群の名前）を定義する。
  - 未結合、または群を選択していないときは「元の動作」と同じ既定値を返す。例：choose は `choice` を0のまま、capture は空のスコープ、force・admit・general は `false`、forget・release・after_copy は何もしない。
  - `skyrim_fuz_host_queue_selected` と `skyrim_fuz_host_queue_world` は段階化した単位の中で定義されるため、ここでは定義しない。
- 新規 `ios/private-abi/pc_guest_mod_hooks_host.c`（ホスト側）。
  - `pc_guest_open` で、Classicの結合の後に `dlsym` でシンボルを探す。ない場合は正常として扱い、フックを使う機能を `GUEST_HOOKS_MISSING` にする。
  - `pc_guest_select_mod_hooks(uint32_t groups)` は、ゲスト開始前にメインスレッドで一度だけ呼ぶ（`pc_guest_select_classic_skse` と同じ規則）。ゲストに存在しない群を要求したら失敗させる。
- ビルド：`scripts/build_title_guest.py` に `--mod-hooks <群の一覧>` を追加する。指定時は §4.3 の段階化を行い、`exports.txt` に `_skyrim_guest_bind_mod_hooks_v1` を足す。`compile-contract.json` と `result.json` に群とヘッダーのSHA-256を記録し、Info.plistに `PcGuestModHooks`（整数）を書く。`--mod-hooks` は `--classic-skse` と組み合わせて使う前提とし、単独指定は拒否する（Classic側の `VM_LIFECYCLE` と組み合わせるため）。
- 組み立て：`scripts/assemble_title_ipa.py` と `scripts/build-title-host.sh` で、ホストのInfo.plist、ゲストの `PcGuestModHooks`、ヘッダーのSHA-256を照合する。既存の「ABIヘッダー4枚」の照合に新しいヘッダーと上記の試作ヘッダーを加える。
- CMake：`ios/runtime-port/CMakeLists.txt` に `option(SKYRIM_IOS_MOD_HOOKS ... OFF)` を追加し、`SKYRIM_IOS_CLASSIC_SKSE` を必須にする。`ios_build_contract.h` に同じ組み合わせ条件を加える。
- 負担：オフのとき、1か所あたり静的ポインタの読み込みと分岐1回。差し込み先は会話、メニュー、ロード、カメラ状態の変更で、毎フレームではない。
- 試験：`tests/native/test_mod_hooks_binding.c` と `tests/test_mod_hooks_binding.py`（`test_classic_skse_binding.py` と同じ構成）。未結合、二重結合、版や大きさの違い、存在しない群の選択、選択は一度だけ、選択前と未選択の既定値、選択後の転送。`test_build_title_guest.py` と `test_title_assembly.py` に `--mod-hooks` の契約、書き出し、Info.plistの照合を追加する。
- コミット：`Add the optional guest MOD hook interface`

### 4.3 X3 ゲストの段階化をまとめるスクリプト

- 新規 `scripts/mod_hooks_guest.py`。`prepare_sources(sources, destination, groups, state)` を、`classic_skse_guest.py` の `prepare_sources` の後に呼ぶ。
  - `experiments/` の既存の段階化スクリプトをパスで読み込む（`fuz-voice/stage_menu_lift.py` の `prepare_menu`、`stage_world_lift.py` の `prepare_world`、`classic-chargen-precache/test_chargen_precache.py` の `stage_open` など）。各スクリプトは元の単位のSHA-256と差し込み位置を確認する。
  - `generation.json` は `fuz-voice/stage_lift.py` の `GENERATION` と一致させる。
  - 同じ単位を複数の群が変える場合（Fuzの `exe_100.c` はメニューと周囲の会話の両方）、出力が同一であることを確かめる。一致しない場合は停止する。
  - Classicの改名（`f_X` → `f_X_guest`）と同じ単位を変える場合は停止する。重なりがないことはCodexが固定版生成物で確かめる。
  - 段階化した単位のために `-I experiments/fuz-voice` などを追加する。対象は段階化した単位だけ。
- 試験：`tests/test_mod_hooks_guest.py`。合成した単位とハッシュ表で、組み合わせ、同一出力の確認、重なりの拒否、未知の群、`generation.json` の不一致を試す。`SKYRIM_TEST_OWNED_CLASSIC_LIFT` があるときだけ、固定版生成物で全群を組み合わせ、構文検査（`clang -fsyntax-only`）を行う。
- コミット：`Compose reviewed MOD hook stages for the guest build`

### 4.4 X4 VMの世代（ロード・リセット時に古い呼び出しを書き込ませない）

**根拠**：通常のロードはVMの凍結が時間切れになっても進むことがあり、レジストリのリセットでスクリプトのフレームが外れることがある（[設計](SKSE_IOS_PLAN.md)）。ロード時のリバートは `0x8D69A7` → `f_8D5DF0`、スクリプト再読込時のリバートは `0x8D6E16`、VMのロードは `0x8D6B79` → `f_8D3DB0`（[新規ロードの試作](../experiments/classic-fresh-load/README.md)、[ModEventの試作](../experiments/classic-mod-events/README.md)）。

- 新規 `ios/runtime-port/classic_vm_epoch.{c,h}`。

  ```c
  typedef struct { uint64_t epoch; uint32_t registry; } SkyrimVmScope;
  void skyrim_vm_epoch_registered(uint32_t registry);   /* Classicの登録フックの後 */
  void skyrim_vm_epoch_before_revert(unsigned site);    /* 元のリバートの直前 */
  void skyrim_vm_epoch_after_load(int succeeded);       /* 元のVMロードの直後 */
  bool skyrim_vm_scope_current(SkyrimVmScope *out);     /* リバート中はfalse */
  bool skyrim_vm_commit_begin(const SkyrimVmScope *);   /* ロックを取り、世代を確認 */
  void skyrim_vm_commit_end(void);
  typedef void (*SkyrimVmRetire)(void *context, uint64_t old_epoch);
  bool skyrim_vm_epoch_subscribe(SkyrimVmRetire, void *context); /* 起動前に登録、最大8件 */
  ```

  - リバートの直前に同じロックを取り、世代を増やし、「リバート中」にしてから購読者を呼び、元の処理へ進む。`after_load(1)` で次の世代を開く。`after_load(0)` は閉じたままにする。
  - ロックの順序は「このロック → エンジン内部のロック」に固定する。コミット中はエンジンの結果コピー（元の `f_C329E0` など）以外のゲスト呼び出しをしない。リバート側は元のリバートを呼ぶ前にこのロックを取って離すため、逆順にならない。
  - ゲストに `VM_LIFECYCLE` 群がないときは、世代に依存する機能（PapyrusUtilの公開、ModEvent、操作イベント）を `GUEST_HOOKS_MISSING` にする。
- ゲスト群 `VM_LIFECYCLE`：上記3か所の前後に1行ずつ差し込む段階化スクリプトを `experiments/classic-fresh-load` の確認済みハッシュと差し込み位置から作る。`0x8D6E16` の単位のハッシュがリポジトリにない場合は、Codexが固定版生成物から追加するまで、その差し込みは止めて記録する。
- 試験：`tests/native/test_classic_vm_epoch.c` と `tests/test_classic_vm_epoch.py`（`-pthread`、ASan/UBSan、TSan）。実行中の呼び出しの途中でリバートしたらコミットが拒否されること、コミット中のリバートが待つこと、世代が単調に増えること、ロード失敗後も閉じたままであること、別のレジストリを拒否すること、購読者が一度だけ呼ばれること。
- コミット：`Track the Classic VM generation across loads and reverts`

### 4.5 X5 DLLのコード変更要求の一覧（診断）

**目的**：Enhanced Camera、Safety Load、HDTなどが要求するゲーム本体のコード変更を、適用せずに一覧にする。各要求を既知の代替処理（アダプター）に対応付け、未対応のものは場所と理由を表示する。

- `experiments/skse-plugin-vm` の観測モードを一般化する。現在は事前に指定したサイトの5バイトのジャンプだけを観測する（`hook_probe_sites`）。
  - 照合済みの `TESV_unpacked.exe` のセクションを読み取り専用の表示として渡す（アプリは起動時のハッシュ確認ですでにこのファイルを読む。コピーせずに `mmap` する）。
  - ゲーム本体の範囲に対する `VirtualProtect` は観測モードでだけ受け付け、元の保護値を返す。その後の書き込みは適用せずに `{サイト、長さ、種別（E8、E9、その他）、DLL内の行き先}` として記録する。
  - 記録の上限は64件。それを超えたら打ち切りの結果を返す。
- 対応表：新規 `ios/runtime-port/mod_hook_catalog.c`。根拠のあるサイトだけを載せる（PapyrusUtilの `0x6A9E90`、`0x8CAB54`、`0x6F02FF`、先読みの `0x886AD0` など）。行き先の関数名は、実行時にゲストの関数表（アドレス順に並んでいることを `pc_guest_open` が確認済み）を二分探索して求める。
- 表示：「このDLLはゲーム本体の 0x%08X（関数 0x%08X）を変更しようとしました。iOS版では未対応です」など、日英で示す。対応表にあるがゲストに群がない場合は「ゲストの再ビルドが必要」と示す。
- 実行は、利用者がDLL診断を開いたときと、取り込み時の上限付きの確認だけ。ゲーム中には実行しない。
- 試験：`experiments/skse-plugin-vm` の自作DLL生成を使い、読み取り表示内のページを `VirtualProtect` して `E9` を書くDLL、範囲外への書き込み、上限超過、読み取り専用の表示が変わらないことを試す。
- コミット：`Inventory DLL code patch requests without applying them`

## 5. 項目別の作業

### P1 SKSE Classic 1.7.3とSkyUI 5.1

#### P1-1 取り込み時と起動時の必要ファイル確認

- 新規 `ios/runtime-port/mod_requirements.{c,h}`（純C）。

  ```c
  typedef enum { SKYRIM_SKYUI_REQ_OK, SKYRIM_SKYUI_REQ_SKYUI_ABSENT, SKYRIM_SKYUI_REQ_SKSE_SCRIPT_MISSING,
                 SKYRIM_SKYUI_REQ_SKSE_SCRIPT_INVALID, SKYRIM_SKYUI_REQ_SKYUI_ARCHIVE_UNSUPPORTED } SkyrimSkyUIRequirement;
  typedef struct { SkyrimSkyUIRequirement status; unsigned support_present, support_expected; } SkyrimSkyUIRequirements;
  int skyrim_skyui_requirements(const char *const *data_roots, size_t count, SkyrimSkyUIRequirements *out);
  ```

  - `data_roots` は元のData、続いて展開順の有効パッケージ。後のものが勝つ。
  - `SkyUI.esp` があり、`SkyUI.bsa` のヘッダーがLE（104、`skyrim_bsa_version`）であることを確かめる。
  - 勝者の `Scripts/SKSE.pex` を8 MiB以下で読み、`skse_pex_inspect`（`experiments/skse-plugin-vm/classic_pex.c`。libc以外に依存しない）で、スクリプト `SKSE` の大域ネイティブ `GetVersionRelease` があることを確かめる。
  - 補助として `UI.pex`、`UICallback.pex`、`Input.pex`、`StringUtil.pex` の有無を数える（警告のみ）。
  - シンボリックリンクは拒否し、大文字小文字の照合は既存の `skyrim_plugin_child_path` を使う。
- `mod_manager.m`：
  - SkyUI.espを含むパッケージを取り込み、対象プロファイルに付けた後に判定する。SKSEスクリプトがなければ通知する。日本語の例：「SkyUIにはSKSE 1.7.3のスクリプトが必要です。所有しているSKSE 1.7.3のアーカイブをこのプロファイルに追加してください。」
  - SkyUI試験の行に判定結果を表示する。
  - `skyrim_mods_prepare_skyui_trial` で起動時のルートを使って判定する。試験の有効化の条件は変えない。不足時は `launchNotice` に示し、`IOS_CLASSIC_SKYUI_INPUTS skyui=[01] skse_pex=[01] support=<0-4>` を出力する。
- CMake：`SKYRIM_IOS_CLASSIC_SKSE` のとき `mod_requirements.c` と `classic_pex.c` を加える。
- 試験：`tests/native/test_mod_requirements.c`、`tests/test_mod_requirements.py`。PEXは `experiments/skse-plugin-vm/test_classic_pex.py` の生成関数で作る。スクリプトがない、壊れたPEX、`GetVersionRelease` がない、元のDataにだけある、後のパッケージが上書きする、大小文字の違い、シンボリックリンク、SE版のBSA（105）、8 MiB超、別プロファイルのルート集合。Macの `tests/mod-simulator/screens.m` に、SKSEなしでSkyUIを取り込んだときの日英の通知を追加する（Codexで実行）。
- 完了条件：Webのホスト試験が通常とASan/UBSanで合格し、`IOS_CLASSIC_SKYUI_INPUTS` が送信用の許可リストと試験に入る。
- コミット：`Check SkyUI's SKSE scripts at import and launch`

#### P1-2 MCM設定の再読込の回帰を固定する

所有者は通常版22で、MCMの設定変更と再読込が良好だったと報告している。この挙動を変えずに、試験で固定する。

- 新規 `tests/test_classic_skyui_regression.py`。`prepare_classic_skse.prepare` で固定版のSkydreamを一時ディレクトリに段階化し、自作のVMで次を確かめる。
  - 試験オンでは `SKSE.GetVersion/Minor/Beta/Release` が1/7/3/48、オフでは元の値。
  - `Utility.GetINIBool` と `GetINIString` がSkyrim.iniからSkyrimPrefs.iniへ順に探す。`test_classic_skse_ini.py` と重複する場合は追加しない。
  - 同じハンドルで `RegisterForModEvent` と `RegisterForMenu` を2回呼んでも配信は1回。
  - `IOS_CLASSIC_RELEASE` は最初の実呼び出しで1回だけ出る。
- **変えないこと**：Skydreamの登録はリバートで消えない。公式SKSEはリバートで消してから追加セーブ（`MCBR`）で戻すが、このポートには復元がない。SkyUIはロード時に登録し直すため現状で動いている。消す変更は、追加セーブの復元（P3-5）ができるまで行わない。この違いを試験のコメントとSTATUSに記録する。
- コミット：`Pin the SkyUI version, INI and registration behavior`

#### P1-3 画面上端の見切れ

**仮説と判別方法**

| 仮説 | 内容 | 判別 |
| --- | --- | --- |
| H1 | アプリのオーバーレイが上部を覆う。`layoutSubviews` は START（4）と BACK（5）を安全領域の高さの7%の位置に置き、保存、キーボード、設定のボタンを上端から44ptで、`hud` をその下に置く | 「ボタン配置」で START と BACK を下へ動かすか、不透明度を最小にして、カテゴリ列が見えるか |
| H2 | 16:9より横長の画面で、ScaleformのメニューがNoBorder相当に拡大され、上下が切れる | SkyUI試験オフの標準の持ち物画面でも上下が対称に切れるか。下端も切れるか |
| H3 | 描画解像度とドローアブルの縦横比が違う | 起動記録の `IOS_DRAWABLE pixels=` と `IOS_GRAPHICS resolution=` の比 |

- P1-3a（Web）：判別用の診断行 `IOS_OVERLAY_LAYOUT schema=1 view_pt=<w>x<h> safe=<t>,<l>,<b>,<r> top_controls_max_y=<pt> scale=<x100>` を、最初のレイアウト時と変更時に出す（数値だけ）。許可リストと試験を更新する。コミット：`Record the overlay layout for the menu clipping check`
- P1-3b（Codex）：所有者の通常版22の画像と記録で判別する。H1なら記録された位置で重なりを示せる。
- P1-3c（H1が確定した場合だけ実装）：
  - Classic試験のビルドで、Skydreamの `skse_menu_view(name)` が持ち物系のメニュー（InventoryMenu、ContainerMenu、BarterMenu、GiftMenu、MagicMenu、Journal Menu）を開いていると返す間は、START、BACK、上部のボタン、`hud` を隠し、設定ボタンだけを残す。
  - 状態の確認は、既存の `skyrim_mobile_tick` の0.5秒ごとの区間で行う。この関数は `skyrim_ios_pump_events` から呼ばれ、`pthread_main_np` のスレッド（ゲームのメインループと同じスレッド）で動く。`skse_ui_pump` と同じ条件（`pc_guest_classic_skse_active`）のときだけ確認し、毎フレームの処理は足さない。
  - 判定は純C関数 `skyrim_mobile_overlay_menu_mode(bool menu_open, bool editing)` に切り出して試験する。
- H2の場合は、メニューの読み込みで使う拡大方式の呼び出し位置を固定版生成物から特定する必要がある。アプリ側だけでは直せないため、Codexの調査として記録する。

### P2 Fuz Ro D-oh 6.1

#### P2-1 方針の入力（設定と代替クリップ）

- 新規 `ios/runtime-port/mod_fuz.{c,h}`（純C）。
  - プロファイルの展開ビューからFuzのINIを読み、1語当たりの秒数の換算率（既定2）と空の応答の扱いを決める。キー名と既定値は、作者のLE 6.1のソース（[FuzrodohInternals.cpp](https://github.com/shadeMe/Fuz-Ro-D-oh/blob/17092b2562e4fff422448bcf2ee9d0a42b995806/FuzrodohInternals.cpp)）と所有者のアーカイブのINIで確かめてから書く。
  - 1〜10秒の代替クリップのData相対パスを、作者のソースのパスの作り方（同じファイルの137〜206行目）から作る。`.xwm` と `.fuz` の両方の名前の扱いは既存の試作に合わせる。
  - 不正な値（0、負数、範囲外、長すぎる行）は既定値に戻し、診断の理由コードを返す。
- 試験：`tests/native/test_mod_fuz.c`、`tests/test_mod_fuz.py`（INIの異常入力、大小文字、BOM、CRLF、上限）。
- コミット：`Read the Fuz Ro D-oh policy from the selected profile`

#### P2-2 ホスト側の実装

- 新規 `ios/runtime-port/mod_fuz_host.c`。§4.2の `FUZ_MENU` と `FUZ_WORLD` の関数を実装し、`experiments/fuz-voice` の `voice_fallback.c`、`lift_voice_seam.c`、`lift_menu_seam.c`、`lift_world_seam.c`、`lift_resource_lookup.c` をそのままリンクする。
- ゲストメモリ：`SkyrimFuzLiftMemory` をランタイムの `MEM` に対して実装する。範囲はゲストメモリの大きさの中に限り、`replace` は比較してから書く。
- ゲスト関数の呼び出し：`lift_resource_lookup.c` が呼ぶ `f_B08630` と `f_B08970` は、新規 `ios/runtime-port/fuz-runtime-abi/funcs_exe.h` で、ランタイムの `rt_call(c, 0x00B08630u)` などへ送る。`lift_prelude.h` もこのディレクトリに置き、ランタイムの `MEM`、`M32`、`M8` を使う。直接呼び出しと同じ結果になることは、Codexが固定版の資源検索の試験をこの経路で実行して確かめる。
- スタックの範囲：呼び出し元のCPUの `fs_base` からTEBの `+4`（StackBase）と `+8`（StackLimit）を読み、ランタイムのスレッドスロットの範囲（`GUEST_STACK_BASE + slot * GUEST_STACK_SIZE`）と一致するときだけ使う。`nested_bytes` は初期値64 KiBとし、Codexが固定版の試験で必要量を測って確定する。
- 同期：追跡表（メニュー、周囲の会話）は1つのミューテックスで守る。呼び出しは会話のときだけ。
- 世代：§4.4の購読者として、リバート時に追跡表を空にする（ロード後に同じアドレスが再利用されても古い印が残らない）。
- 有効化：機能 `FUZ` が選択され、ゲストに `FUZ_MENU|FUZ_WORLD` があり、`VM_LIFECYCLE` があるときだけ群を選択する。どれかが欠けたら群を選択しない（ゲストの既定値で元の動作）。
- 試験：`tests/test_mod_fuz_host.py`（自作の `MEM` と、ゲスト関数を模擬する `rt_call`）。
  - オフでは何も書かない。
  - 元の音声がある場合は置き換えない。
  - 音声がなく代替クリップがある場合は置き換える。
  - 代替クリップがない、または検索が失敗した場合は元のパスに戻る。
  - 同じ文面で別の台詞には字幕の印を付けない（既存の試作の確認をホスト経路でも行う）。
  - リバートで印を消す。
  - 範囲外のメモリ、小さすぎるスタック、同じスレッドからの再入を拒否する。
- コミット：`Connect the Fuz Ro D-oh hooks to the app runtime`

#### P2-3 画面と診断

- 「Fuz Ro D-oh 6.1（無音声の台詞に字幕と口の動き）」の機能スイッチ。状態の文は「DLLを認識できません」「ゲストの再ビルドが必要です」など。
- `IOS_FUZ schema=1 enabled=[01] substituted=<n> kept=<n> missing=<n> errors=<n>` を終了時と1分ごとに出す（会話の発生時に数えるだけ）。
- コミット：`Offer Fuz Ro D-oh per profile with diagnostics`

#### P2-4 Codex引き継ぎ（固定版生成物）

```sh
cd pc-aot/experiments/fuz-voice
SKYRIM_TEST_OWNED_CLASSIC_LIFT=<private-generation> SKYRIM_TEST_SANITIZERS=1 python3 -B -m unittest -v test_voice_fallback
cd ../.. && SKYRIM_TEST_OWNED_CLASSIC_LIFT=<private-generation> python3 -B -m unittest -v tests/test_mod_hooks_guest.py
python3 scripts/build_title_guest.py private/<stage> --classic-skse --mod-hooks fuz_menu,fuz_world,vm_lifecycle
```

### P3 PapyrusUtil 3.3、Wearable Lanterns 4.0.2、追加セーブ

段階：P3-1（世代）→ P3-2（JsonUtilの公開）→ P3-3（ModEvent）→ P3-4（操作イベント）→ P3-5（StorageUtilと `.skse`）。ランタンの設定変更と再起動後の復元はP3-2で通す。

#### P3-1 世代の接続

§4.4を実装する。`skyrim_vm_epoch_registered` は、Skydreamの `skse_register_natives(CPU *, uint32_t registry)` の先頭で呼ぶ（`skse_entry.c` の `f_8F9720` が元の登録の後にレジストリを渡して呼ぶ）。`prepare_classic_skse.py` に段階を1つ追加して差し込む。

#### P3-2 JsonUtilのゲームプレイ中の公開

- 新規オプション `SKYRIM_IOS_CLASSIC_PAPYRUSUTIL`（既定OFF、`CLASSIC_SKSE` と `SKSE_DLL_PROBE` が必須）。既存の `SKYRIM_IOS_CLASSIC_SETTINGS_PROBE` は起動試験として残す。
- 新規 `ios/runtime-port/classic_papyrusutil.c`。
  - 起動時、`skyrim_mobile_prepare_mods` の後、ゲスト開始前に実行する。機能 `PAPYRUSUTIL` が選択され、ゲストに `VM_LIFECYCLE` があるときだけ、`skyrim_mods_acquire_papyrus_inputs` で `skse_classic_settings_open` を開く。
  - `ClassicValueRequest` で、Wearable Lanternsが使うJsonUtilの関数を公開する。対象は、[WL 4.0.2のソース](https://github.com/chesko256/WearableLanterns/blob/ebf678e6a17c5f3ee0723e851e1ff4497e77fe19/Scripts/Source/_WL_SkyUIConfigPanelScript.psc)のJsonUtilとStorageUtilの呼び出しを数えて決め、表に記録する。`classic_settings` の7メソッドにないスカラー関数が必要なら、メソッドを追加し、所有者のDLLの試験（Webではスキップ）と自作DLLの試験を足す。配列の関数は対象外として診断に出す。
  - `owns_frame`：既存のフレーム確認（戻り先 `0xc46e61`、レジストリ）に加えて、`skyrim_vm_scope_current` の世代が呼び出し開始時と同じであることを確認する。
  - `commit`：`skyrim_vm_commit_begin` で世代を確かめてから `skse_classic_settings_commit` を行い、`skyrim_vm_commit_end` で離す。世代が変わっていたら `discard` にする（結果も設定ファイルも書かない）。
  - リバートの購読者で、その世代の未確定の呼び出しを捨てる。
  - 起動時の事前読み込み（自動読み込みの設定）は既存の起動試験と同じ手順を使う。
  - 終了時（`skyrim_ios_runtime_main` の戻り後）に `skse_classic_values_retire` と `skse_classic_settings_close` を呼ぶ。
- 試験（Web）：`tests/test_classic_papyrusutil.py`。自作DLLと自作VMで次を確かめる。
  - 呼び出し途中のリバートで古い結果を書かない。
  - ロード失敗後は呼び出しを受け付けない。
  - 保存の確定を拒否されたら、結果と既存ファイルを残す。
  - 別のプロファイルの設定が見えない。
  - 書き込み途中に終了したら、次の起動で直前の完全なファイルを読む。
  - 起動を繰り返しても同じ値を復元する。
  - 所有者のDLLの試験は `SKYRIM_TEST_OWNED_PAPYRUS_ZIP` と `SKYRIM_TEST_OWNED_LE_EXE` があるときだけ実行する。
- 診断：`IOS_PAPYRUSUTIL schema=1 published=<n> calls=<n> refused_stale=<n> commits=<n> discards=<n> errors=<n>`。
- コミット：`Publish PapyrusUtil JsonUtil natives during play`

#### P3-3 ModEvent（ランタンの燃料・花粉メーター）

- `experiments/classic-mod-events/mod_event.c` をClassicのランタイムに加える。`prepare_classic_skse.py` に、`ModEvent.Create`、`PushBool`、`Send`、`Release` の記述子と `skse_system_run` への分岐を追加する。
- 既存の `prepare` は、段階化済みのファイルが途中の段階のどれかと一致することを確かめる。新しい段階を足すときは、その一覧にも加える。
- 対象外の型（`PushInt` など）は登録せず、MOD一覧の「不足するSKSE関数」に出す。
- 世代の購読者で、パケットの表と登録を退役させる。
- 試験：`experiments/classic-mod-events/test_mod_events.py` のうち固定版生成物を使わない部分を、新しい経路で実行する。
- コミット：`Register Classic ModEvent statics for boolean packets`

#### P3-4 操作イベント（ランタンの長押し切り替え）

- `experiments/classic-control-events` の9つのメソッドの記述子と、`skse_input.c` のGFX呼び出し直前の観測を、`prepare_classic_skse.py` に追加する。スリープ時の後始末（`0x8D657A`）は `VM_LIFECYCLE` の群に含める。
- 試験：`test_control_events.py` と `test_control_registration.c` のうち、Webで実行できる部分。
- コミット：`Deliver Classic control events to registered scripts`

#### P3-5 StorageUtilと `.skse`（追加セーブ）

- ゲスト群 `SAVE_LIFECYCLE`：保存の差し込み位置 `0x6816B2`、最終の名前変更 `0x680317`、新規ロード `0x682220` と `0x682275`。根拠は `classic-save-publish`、`classic-fresh-load`、`classic-runtime-save` の各試作。
- ホスト：既存の `skse-cosave` の保存庫（`skse_pair_*`）、常駐DLLの直列化コールバック、`copy_pair` を組み合わせる。ベースの `.ess` を公開し、VMの保存が成功したときだけ追加セーブを確定する。ロード時は、同じ世代と内容のハッシュが一致したときだけ復元する。
- 先読み済みのストリーム（`manager + 0x30`）の経路では復元しない（試作の結論のとおり）。
- この段階は固定版生成物と実機の確認が多いため、Webでは自作の範囲の試験と、ホスト側の結合までを行う。
- コミット：`Stage StorageUtil companion saves behind the save lifecycle hooks`

### P4 RaceMenu 3.4.5、ECE 1.59、NetImmerse Override 3.4.4

#### P4-1 `.jslot` の読み取り

- 新規 `ios/runtime-port/mod_racemenu_preset.{c,h}`（純C、上限1 MiB、深さと要素数に上限のあるJSONの一部だけの解析）。
  - `morphs.default.morphs` の19個のfloat（`TESNPC::FaceMorphs::option[]` の順）を取り出す。
  - キー名は、作者のLEのソースのJSON書き込み処理（[MorphHandler.cpp 2091〜2107行目](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/MorphHandler.cpp#L2091-L2107)）を読んで確かめる。
  - 有限値以外、要素数の違い、重複キーは拒否する。
  - `morphs.custom`、`overrides`、体型のモーフ、髪や色合いは「未反映」の一覧として返す。
  - 最初の対象は鼻の高さ（`morphs.default.morphs[1]`、`kMorph_NoseDownUp`）。
- 試験：自作のJSONで、正常、切り詰め、巨大、入れ子が深すぎる、NaN、要素不足、Unicodeのエスケープ、BOM。
- コミット：`Parse RaceMenu presets for the standard face sliders`

#### P4-2 プロファイルで選んで、次にキャラクターメニューを開いたときに反映

- 画面：プリセットのパッケージに「このプリセットを適用」を追加し、`profile[@"pendingPreset"] = {package, file, sha256}` を保存する。起動時に固定し、反映後に消す。
- ゲスト群 `CHARGEN_SLIDER`：キャラクターメニューの更新（vtableの5番目 `f_885980`、[先読みの試作](../experiments/classic-chargen-precache/README.md)）で、反映待ちがあるときだけホストを呼ぶ。標準の連続スライダーの処理を、メニューの所有スレッドで、メニューの世代とプレビュー中のアクターを確かめてから呼ぶ。
  - 呼び出す関数、引数の配置、スライダーの範囲の読み方は固定版生成物から確定する（Codex）。**確定するまで、この群は作らない。**
  - Webでは、ホスト側の判定（値の範囲、世代、一度だけの反映）を純Cで試験する。
- 永続：標準の顔のモーフはプレイヤーの `TESNPC` として `.ess` に保存される。そのため追加セーブなしで保存後と再ロード後に残るはずで、実機で確かめる。
- 負担：キャラクターメニューを開いている間だけ、1フレームに1回の読み込み。ゲームプレイ中は呼ばない。
- 表示：RaceMenuのDLLの機能（拡張スライダー、オーバーレイ、体型のモーフ）は未対応で、標準スライダーのプリセットだけを反映すると明示する。
- コミット：`Apply a selected RaceMenu preset to the standard sliders`

#### P4-3 ECE 1.59

- 取り込み済みの `.slot` は残す。ECE 1.59の公式 `humanraces_slider.ini` とNordの同梱プリセットで、標準 `NoseUpDown` のIDが12であることを確認した。
- `mod_ece_slot.{c,h}` は検証用の読み取り処理。上限1 MiBのスロットからID 12の生の値を取り出し、壊れた行やID 12の重複を拒否する。作者のプリセットと、鼻の高さだけを変えたコピーをホストで比較する。
- WindowsのECEで同じキャラクターのスロットを2つ書き出し、IDと値の方向・範囲を確認する。その後、P4-2の経路を使う。RaceMenuとは別のプロファイルでiPhoneの反映を試すため、現行UIの表示は取り込みまで。

#### P4-4 NiOverrideの重複

- §4.1で、2つの有効なパッケージが `SKSE/Plugins/nioverride.dll` を持つ場合に `DUPLICATE` とする。勝者（後のパッケージ）とPEの版を表示する。例：「RaceMenu 3.4.5に含まれるNiOverrideとNetImmerse Override 3.4.4の両方が有効です。後から有効にした方が使われます」。
- 試験：自作のPEバージョンリソースを使う。
- コミット：`Report duplicate NiOverride providers`

### P5 ゲームデータ、骨格、生成済み素材

| 作業 | 実装・試験（Web） | 実機（Codex） |
| --- | --- | --- |
| P5-1 USLEEP 3.0.16と3つのDLC | `test_mod_plugins.py` に、Skyrim、Update、Dawnguard、HearthFires、Dragonbornをマスターに持つ自作ESPを追加する。全部ある、1つオフ（不足の表示）、明示的にオフにしたDLC、実ファイルがないDLC、セーブの確認での不足表示 | コピーしたセーブを読み込み、USLEEPが変更するレコードを1つ選んで確かめる |
| P5-2 XPMS 1.96とRRF 1.9 | 自作パッケージで、`skeleton.nif` と `skeleton_female.nif` の勝者が競合レポートに出ること。別プロファイルのビューが混ざらないこと。2つの骨格パッケージがあるときの表示（新規）を追加する | 別プロファイルで骨格の選択と動きの切り替え |
| P5-3 CBBE 3.4fとUNP 1.2 | 2つのプロファイルで同じ `femalebody_*.nif` が別の勝者になること。NIFのユーザー版83と100の判定（既存）を表示に使う | 各プロファイルで体型を確かめる |
| P5-4 BodySlide 6.1.0とFNIS 7.6の生成物 | 取り込み時に `generatedFor = {profile, 有効なパッケージの一覧のSHA-256}` を記録し、後でそのプロファイルの構成が変わったら再生成を促す。FNISの出力として必要なファイルは、FNIS 7.6の公開文書で確かめて表にする。64ビットHKXの警告（既存）を維持する | 所有者がPCで生成した出力を取り込み、該当プロファイルで体型と動きを確かめる |
| P5-5 XPMSE 5.06 | §4.1で、FNISの生成物とSKSEスクリプトの有無を確認する。NiOverrideが任意であることを表示する | HDTとの組み合わせはP6-5 |

コミット：`Check DLC masters, skeleton winners and generated assets per profile`（試験と表示の単位で2〜3個に分けてよい）。

### P6 髪、カメラ、ロード、物理

| 作業 | 内容 | Webで行うこと | 止まる条件・Codex |
| --- | --- | --- | --- |
| P6-1 ShowRaceMenu Precache Killer 1.3 | 有効なプロファイルにあるLE 1.3のDLLを照合したら、ゲスト群 `CHARGEN_PRECACHE` で元版と同じ先読みルーチン入口から戻る。`experiments/classic-chargen-precache` の段階化をX3に組み込む | ゲスト側の既定値、3つの呼び出し元とレジスタを維持すること、ホストの版とセットアップの判定 | 同じ髪型を選んで、表示と初回の所要時間を実機で比較する |
| P6-2 KS Hairdos 1.10とApachii Sky Hair 1.6 Full | データだけ。マスター、LEのBSA、ヘッドパーツを含むプラグインの読み込み | プラグインとBSAの判定の試験 | 実機で髪の切り替え（標準の先読みとP6-1の両方） |
| P6-3 Enhanced Camera 1.4 | DLLはフレームごとの描画処理を変更するため、代替処理なしでは動かない | X5で要求の一覧を表示する。ゲスト群 `CAMERA_OBSERVE` で `SetCameraState` の後に `experiments/classic-camera` の読み取りを呼び、`IOS_CAMERA_STATE slot=<n>` を出力する（状態が変わったときだけ） | 一覧と作者の説明から代替処理を設計するまで機能は「未対応」とする |
| P6-4 Safety Load 1.2 | ロード中の待ちを変更するDLL | X5で要求の一覧を表示する | 要求とロードの経路を固定版生成物で照合してから判断する |
| P6-5 HDT Physics Extensions 14.28 | Havokの物理処理へのフックが大きい | XML、INI、メッシュの取り込みを維持し、DLLは未対応と表示する（X5の一覧つき） | 別の開発として扱う。実機では静止した表示まで |
| P6-6 HDT HighHeels 0.5 | 装備したときに高さを補正し、外したときに戻す | 仕組み（NIFの追加データ、ESP、スクリプト、DLLの要求）を所有者のパッケージで調べる手順を用意する。NIFの追加データを読む場合は、自作のNIFで試験できる読み取り部分だけ先に作る | 装備変更とノード変換の位置は固定版生成物から確定する。確定まで止める |

コミット：`Skip the initial character menu precache when the DLL is present`、`Report camera state transitions for Enhanced Camera checks`、ほかは表示と試験の単位。

## 6. P7 STATUS.mdと引き継ぎ

- `docs/STATUS.md` の先頭に日付つきの節を追加し、第一弾の全項目を表にする。列は「実装の範囲」「通った試験（件数、Clang、ASan/UBSan）」「iPhoneで残る確認」「止まっている場合の理由」。対象は、SKSE、SkyUI、Fuz、PapyrusUtil、Wearable Lanterns、追加セーブ、RaceMenu、ECE、NiOverride、Precache Killer、KS Hairdos、Apachii、HDT PE、HDT HighHeels、XPMSE、XPMS、RRF、CBBE、UNP、BodySlide、FNIS、USLEEPとDLC、Enhanced Camera、Safety Load。
- `docs/SKSE_IOS_PLAN.md` の「Current host checks」を更新する。
- 最終報告に、ブランチ名、コミット一覧、実行した試験のコマンドと件数、§8の手順を書く。

## 7. 試験の一覧（追加するもの）

| 区分 | 試験 | 観点 |
| --- | --- | --- |
| 異常入力 | `test_mod_features`、`test_mod_requirements`、`test_mod_fuz`、`test_mod_racemenu_preset`、X5のDLL | 壊れたPE、PEX、INI、JSON。上限超過、シンボリックリンク、大小文字の別名、NaN |
| プロファイル切り替え | `test_mod_features`、`test_classic_papyrusutil`、`test_mod_archive` | 起動時に固定、別プロファイルの設定が見えない、ビューが混ざらない |
| 終了 | `test_classic_papyrusutil`、既存の `skse-profile-json`、`copy_pair` | 書き込み途中の終了後に直前の完全な状態が残る |
| 保存失敗 | `test_classic_papyrusutil`、機能スイッチの保存失敗（Mac UI） | 確定の拒否で結果とファイルが残る。スイッチが元に戻る |
| ロードとリセット | `test_classic_vm_epoch`、`test_mod_fuz_host`、ModEvent | 古い呼び出しが書き込まない。追跡表の消去 |
| 既定オフ | 全機能 | オフ時は何も書かず、元の経路を通る |
| ゲストABI | `test_mod_hooks_binding`、`test_build_title_guest`、`test_title_assembly` | 未結合、二重結合、版違い、群の不一致 |

各試験は、通常、`SKYRIM_TEST_SANITIZERS=1`、スレッドを使う試験は `SKYRIM_TEST_SANITIZERS=thread` でも実行する。

## 8. Codex側の作業と実機確認手順

### 8.1 所有者のアーカイブで確かめる値

設定 → MOD → 詳細 → DLL互換性で、各DLLの `Query` の名前と版、SHA-256、未対応の呼び出し、X5の要求一覧を記録し、§4.1の表へ入れる。DLLの中身は記録しない。

### 8.2 固定版生成物での確認

```sh
cd pc-aot
SKYRIM_TEST_OWNED_CLASSIC_LIFT=<gen> python3 -B -m unittest -v tests/test_mod_hooks_guest.py
python3 -B experiments/classic-fresh-load/test_fresh_load.py --generation <gen> --sanitizers --compose
python3 -B experiments/classic-mod-events/test_mod_events.py --generation <gen> --sanitizers
(cd experiments/classic-chargen-precache && SKYRIM_TEST_OWNED_CLASSIC_LIFT=<gen> python3 -B -m unittest -v test_chargen_precache)
python3 scripts/build_title_guest.py private/<stage> --classic-skse --mod-hooks <群>
```

確かめること：
- Classicの改名と段階化の単位が重ならない。
- `0x8D6E16` の単位のハッシュ。
- `rt_call` 経由の資源検索が直接呼び出しと一致する。
- `nested_bytes` の必要量。
- キャラクターメニューの標準スライダーの呼び出し位置と引数。

### 8.3 Apple SDKとシミュレーター

`SKYRIM_IOS_CLASSIC_SKSE=1 SKYRIM_IOS_SKSE_DLL_PROBE=1 SKYRIM_IOS_MOD_HOOKS=1 bash scripts/build-title-host.sh`、`bash experiments/skse-plugin-vm/test-apple-sdk.sh`、MOD画面のシミュレーター試験（日英）。

### 8.4 実機での比較（同じIPA、MOD無効のプロファイルとの比較）

1. プロファイルA（MODなし）とプロファイルB（確認するMODだけ）を用意し、同じコピーしたセーブを使う。
2. 各プロファイルで3回ずつ次を測る。
   - 起動：記録の `sdl-application-entry` から最初のフレームまで。
   - フレーム時間：同じ場所で5分間の `IOS_PERF` の中央値と最大値（`scripts/analyze-frame-timing.py`）。
   - メモリ：`IOS_DEVICE_STATE` の空き容量。
   - セーブ：クイックセーブの所要時間と、`.ess` と `.skse` の作成。
3. MODごとの確認：

| MOD | 観察すること |
| --- | --- |
| SKSEとSkyUI | 持ち物一覧とMCMをタッチで開く。MCMの設定を変えて保存し、終了して読み込み直す。Error Code 1が出ない。上端の表示（P1-3） |
| Fuz | 無音声の会話で字幕の時間と口の動き（メニューと周囲の会話）。通常の音声つきの台詞が変わらない。同じ文面の別台詞に字幕が出ない |
| PapyrusUtilとWL | MCMでランタンの設定を変え、プロファイルに保存し、アプリを再起動して戻る。ランタンを装備して点灯する。メーターの表示。長押しで切り替わる |
| 追加セーブ | `.skse` が作られ、別プロファイルへのコピー後も値が戻る |
| RaceMenu | プリセットを選び、キャラクターメニューを開いて鼻の高さが変わる。保存、再ロード、再起動の後も残る |
| ECE | 別プロファイルで同じ確認（形式が確定した後） |
| Precache Killerと髪 | キャラクターメニューを初めて開くまでの時間と、髪を変えたときの最初の表示までの時間を、機能のオンとオフで比べる |
| 骨格と体型 | 各プロファイルで骨格、体型、動きの切り替え |
| USLEEP | DLCのマスターが解決され、選んだレコードが変わっている |
| Enhanced Camera | `IOS_CAMERA_STATE` と画面で、主観視点の体の表示（未対応なら未対応と確認） |

## 9. 必要な入力（Web環境にないもの）

- 所有者の配布アーカイブ：SKSE 1.7.3、SkyUI 5.1、Fuz Ro D-oh 6.1、PapyrusUtil 3.3、Wearable Lanterns 4.0.2、RaceMenu 3.4.5、ECE 1.59、NetImmerse Override 3.4.4、ShowRaceMenu Precache Killer 1.3、KS Hairdos 1.10、Apachii Sky Hair 1.6 Full、HDT Physics Extensions 14.28、HDT HighHeels 0.5、XPMSE 5.06、XPMS 1.96、Realistic Ragdolls and Force 1.9、CBBE 3.4f、UNP 1.2、USLEEP 3.0.16、Enhanced Camera 1.4、Safety Load 1.2。
- 所有者がPCで生成したBodySlide 6.1.0とFNIS 7.6の出力。
- 公式DLCの3つのESMとBSA。
- 固定版生成物（`generation.json` が `fuz-voice/stage_lift.py` の `GENERATION` と一致するもの）と `TESV_unpacked.exe`。
- コピーしたセーブ。同じキャラクターで1つのスライダーだけを変えた `.jslot` の組と `.slot` の組。
- Mac（Xcode 26.6、iPhoneOS SDK）と、所有者のiPhone 17 Pro Max。

## 10. コミット計画（この順に、1つずつ試験してからコミットする）

1. `Initialize the resolved MOD slot for GCC 13`（P0-1）
2. `Check SkyUI's SKSE scripts at import and launch`（P1-1）
3. `Pin the SkyUI version, INI and registration behavior`（P1-2）
4. `Record the overlay layout for the menu clipping check`（P1-3a）
5. `Detect first-wave MOD features per profile`（X1）
6. `Add the optional guest MOD hook interface`（X2）
7. `Compose reviewed MOD hook stages for the guest build`（X3）
8. `Read the Fuz Ro D-oh policy from the selected profile`（P2-1）
9. `Track the Classic VM generation across loads and reverts`（X4、P3-1）
10. `Connect the Fuz Ro D-oh hooks to the app runtime`（P2-2）
11. `Offer Fuz Ro D-oh per profile with diagnostics`（P2-3）
12. `Publish PapyrusUtil JsonUtil natives during play`（P3-2）
13. `Register Classic ModEvent statics for boolean packets`（P3-3）
14. `Deliver Classic control events to registered scripts`（P3-4）
15. `Parse RaceMenu presets for the standard face sliders`（P4-1）
16. `Report duplicate NiOverride providers`（P4-4）
17. `Check DLC masters, skeleton winners and generated assets per profile`（P5）
18. `Inventory DLL code patch requests without applying them`（X5）
19. `Skip the initial character menu precache when the DLL is present`（P6-1）
20. `Report camera state transitions for Enhanced Camera checks`（P6-3）
21. P3-5、P4-2のゲスト側、P1-3cは、根拠がそろった時点で個別にコミットする。
22. `Record the first-wave MOD status`（P7）

各コミットの前に、`python3 scripts/check-public-tree.py` と関連試験を実行する。コミットメッセージの最後には、会話で指定された帰属表示の行を付ける。

## 11. 実装時の注意

- ルートの `.gitignore` は「すべて無視して許可したものだけ追跡」の方式。新しいファイルは `git check-ignore -v <file>` で確かめ、既存の慣例どおり `git add -f` するか、`.gitignore` に許可の行を足す。`vendor/`、`local/`、`private/` の中は追加しない。
- `prepare_classic_skse.py` は、固定版のSHA-256（`PINNED`）と、途中の段階の一致を確かめる。段階を足すときは、許容する途中の段階の一覧にも加える。
- `build_title_guest.py` のゲストは、`SYSTEM_SYMBOLS` 以外の未定義シンボルを持てない。ゲスト側のファイルで `strlen` などを使わない。
- 試験はすべて `-Werror` でコンパイルされる。Clangを既定にし、GCCでも確かめる。
- 文言は日英の両方を書く。シミュレーター試験が文言と識別子を照合するため、既存の文言と識別子を変えない。
- アドレスは、リポジトリ内の試作のREADMEか試験に根拠があるものだけを使う。新しいアドレスが必要なら、その作業を止め、Codexへの依頼として記録する。
- 実物のMOD、ゲームのファイル、生成コードを試験の入力として持ち込まない。所有者の入力が必要な試験は、既存と同じく環境変数があるときだけ実行し、ないときはスキップの理由を表示する。
