# 第一弾MODの引き継ぎ記録（2026-11-21）

この記録はClaudeのLinux環境で確認した当時の範囲を示す。現在の実装、対応範囲と実機確認状況は [STATUS.md](STATUS.md) と [第一弾計画](MOD_FIRST_WAVE_PLAN.md) を参照。

作業ブランチ `claude/skyrim-ios-first-wave-mods-rfhjq7`（元は `codex/first-wave-apple-check` の `3bf2aaf`）。この環境には固定版生成物、`TESV_unpacked.exe`、所有者のMOD、Apple SDK、実機がない。そのため、下の項目はコミット済みの実装と試験を土台に、入力が揃った環境で次を実行する。入力や所有者のデータはリポジトリに入れない。

## 共通の準備

```sh
cd pc-aot
mkdir -p vendor && git init -q vendor/skydream
git -C vendor/skydream remote add origin https://github.com/meizfl/skydream.git
git -C vendor/skydream fetch --depth=1 origin ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a
git -C vendor/skydream checkout -q --detach FETCH_HEAD
export SKYRIM_SKYDREAM_SOURCE=$PWD/vendor/skydream SKYDREAM_SOURCE_DIR=$PWD/vendor/skydream
# 固定版生成物と所有者の入力があるとき
export SKYRIM_TEST_OWNED_CLASSIC_LIFT=<private-generation> SKYRIM_TEST_OWNED_LE_EXE=<TESV_unpacked.exe>
```

`scripts/fetch-mod-codecs.sh`（bcdec、stb）はこの環境では実行できなかった。テクスチャ変換を使う試験はMacか、許可された環境で実行する。

## 項目別

| 項目 | リポジトリで固定済み | 必要な入力 | 実行コマンド | 期待結果 |
| --- | --- | --- | --- | --- |
| 1 SKSE / SkyUI | バージョン値・INI・`IOS_CLASSIC_RELEASE` 1回（`test_classic_skse_ini`）、再登録の置換・初期メニュー状態・リバートで登録を消さない・コールバック再登録の安全性（`test_classic_skyui_regression`）。メニュー表示中の操作部品の判定 `skyrim_mobile_overlay_menu_mode` | 所有者のビルド22の画像と記録、実機 | 起動記録の `IOS_OVERLAY_LAYOUT`、`IOS_DRAWABLE pixels=`、`IOS_GRAPHICS resolution=` を取得 | H1（操作部品の重なり）なら `top_controls_max_y` が上端のカテゴリ列と重なる。H1が確定したら `skyrim_mobile_tick` の0.5秒区間で `skse_menu_view` を確認し、`skyrim_mobile_overlay_menu_mode` で隠す。H2/H3は固定版生成物の調査 |
| 2 Fuz / Precache Killer / 髪型 | 設定、音声選択、字幕、先読み入口の試験 | 固定版生成物、Fuz 6.1、Precache Killer 1.3、髪型パッケージ | `cd experiments/fuz-voice && SKYRIM_TEST_SANITIZERS=1 python3 -B -m unittest -v test_voice_fallback`、`(cd experiments/classic-chargen-precache && python3 -B -m unittest -v test_chargen_precache)` | 全件合格（この環境ではFuz 8件中4件がスキップ） |
| 3 PapyrusUtil / WL / 追加セーブ | 実DLLの保存と復元、通知と追加セーブの照合 | 同上と所有者のPapyrusUtil、コピーしたセーブ | `python3 -B experiments/classic-fresh-load/test_fresh_load.py --generation <gen> --sanitizers --compose`、`python3 -B experiments/classic-mod-events/test_mod_events.py --generation <gen> --sanitizers` | 合格。実機はMCMで設定を変え、アプリを再起動して値が戻ること |
| 4 RaceMenu / ECE / NiOverride | `.jslot` と `.slot` の読み取り、鼻の高さ（ID 12） | 同じキャラクターで1つだけ値を変えた `.jslot` と `.slot` の組 | `python3 -B -m unittest -v tests/test_mod_racemenu_preset.py tests/test_mod_ece_slot.py` | 実書き出しで値の向きと範囲が確定 |
| 5 骨格・体型・生成物・マスター | 競合、骨格の勝者、CBBE/UNP、生成物、マスター順の試験 | 所有者のBodySlide/FNIS出力、公式DLC | `python3 -B -m unittest discover -s tests -p 'test_mod_*.py'` | 合格。実機は各プロファイルで体型と動きを比較 |
| 6 Camera / Safety Load / HDT | カメラ観測、HighHeels高さ補正、タスクキュー通知の試験 | 固定版生成物 | `(cd experiments/classic-highheels && SKYRIM_TEST_OWNED_CLASSIC_LIFT=<gen> python3 -B -m unittest -v test_highheel_height)` | 全件合格。呼び出し位置・スレッド・参照の寿命は実機の記録で確定 |

実機の比較手順は [MOD_FIRST_WAVE_PLAN.md §8.4](MOD_FIRST_WAVE_PLAN.md#84-実機での比較同じipamod無効のプロファイルとの比較) に従う。
