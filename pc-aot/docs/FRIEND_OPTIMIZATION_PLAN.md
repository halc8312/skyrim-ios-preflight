# 友人の提案を検証する実施計画

2026-11-14。対象はPC版1.9.32由来のSkyrimIOS。現在の端末build5、元ゲーム・セーブ・画質を基準にする。新GOまでアプリ／private guest／クラウドbuild、push、署名、実機更新はしない。自作fixtureのhostコンパイルは実施する。

## 1. 保存と瞬間停止を区別する

- 既存のread／pipeline／frame gapと同じ単調時計で、WriteFile／WriteFileExの実syscallを計測する。返り値、errno、guest last error、出力byte数、file位置、APCの順序を変えない。
- save候補ファイルへの書込みと、一般のcache／logへの書込みを区別する。ファイル名・内容は新しい計測へ出さない。分類不能はunknownで、autosaveと断定しない。
- 小さなwriteの集まりやwrite間の処理もあるため、save候補handleの最初のwriteからcloseまでの区間も観測候補にする。これは保存全体のCPU時間でも永続化完了でもない。
- 既存の固定量のhistogram／50ms以上の区間記録を利用する。追加のファイルI/O、heap割当、待機を記録関数へ持ち込まない。
- 自作入力で通常・短いwrite・失敗・無効handle・overlapped callbackの旧新一致を確認する。実機での比較は同save／画質／cap／warm cacheで行い、autosaveのON/OFF以外を固定する。
- 採用条件：保存に重なる停止が反復して減る。保存していないcamera／magicの停止が残れば、別原因として追う。オートセーブの既定値は勝手に変更しない。

## 2. SPOの対象・依存・描画差分を確認する

- 作者のclassic版のみを候補にし、選択した実ファイルのmasterを確認する。現在の導入一覧はSkyrim.esm／Update.esmのみ。
- MODは隔離した解析用ディレクトリへ取得できた場合だけ調べる。原Data・plugins.txt・load order・セーブには入れない。認証や利用制限を回避しない。
- CELL／REFR等の変更を整理し、occlusion以外の照明／fog／配置変更、必要DLC、版違いを判定する。作者の説明だけで全差分を確認済みとしない。
- 実機比較は1室・同視点・同capから始める。draw数／frame gapの改善と、見える物・反射・光が保たれることを両方確認する。見える物が消える場合は不採用。
- 適合する実ファイルが得られない場合は依存と比較条件までを保存し、互換性や効果を合格にしない。

## 3. Sheson相当の対策は内部poolの証拠を先に得る

- 公式classic SKSEの公開sourceから、対象pool・設定・測定に必要な構造を確認する。Windows DLLやx86 code patchは導入しない。
- Rootだけが固定の所有入力と既存解析結果を照合する。版・構造・入口が証明できる範囲を記録し、推測したアドレスの読取りやnative置換は接続しない。
- ゲーム内部pool、runtimeの32bit address領域、iOS全体予算を別の値として扱う。OS余裕だけでは内部pool不足を否定しない。
- 将来の計測はpoolごとの使用量／最大値／容量／割当失敗／拡張待ちが対象。上限近傍と停止の対応が取れるまで容量を増やさない。
- 必要な場合だけ局所移植を設計する。割当結果・alignment・所有権・失敗挙動・32bit範囲を維持する。

## 完了と保留の区別

今回PC内で完了できるのは、保存I/Oの計測候補と自作回帰確認、取得可能な公開source／MODの互換性調査、内部pool計測の接続条件の整理。FPS／温度／表示一致／autosave因果の判定は実機比較まで保留する。SKSE一式、INI一括変更、RAM権限やFPS capの重複実装は行わない。

根拠は[調査記録](PERFORMANCE_WORK.md#2026-11-14-友人のpc版対策案未実装の採否調査)。実施結果は同記録と本計画へ追記する。

## 今回の到達点

| 段階 | 完了した範囲 | 残る条件 |
| --- | --- | --- |
| 保存I/O・設定 | 計測に加えてKeep current／Off／OnをSettingsへ実装。次回起動時の4項目上書きと他設定保持を実INI処理のfixtureで確認 | Apple／UIKitの実行、実機の保存ON/OFF比較。保存前のserializationは未計測 |
| SPO | 正規classic 1.6本体のみ版を取得・隔離解析。masterは適合。677遮蔽marker以外の物体追加、既存配置／CELL設定差分も確認 | 元版の一括導入は見送り。室内ごとの依存解析と表示一致・実機比較が必要 |
| 内部heap | 公式SKSEの公開sourceを取得し、容量変更2位置を固定所有入力の命令と照合。5入口／451命令位置を限定確認 | live used／high-water／growthの値とwriterのlock契約。pool不足の証拠はまだない |

### 次回実機比較

新GO後の同一候補版で、同じsave／画質／cap／cache条件を固定する。まず静止→camera→攻撃／魔法の基準区間を記録し、次に保存が発生する同じ操作（室内出入り等）をautosave ON/OFFで比較する。手動保存の区間は別に記録する。設定変更は本人が比較を始める段階だけで行う。試行順やcache温まりを入れ替えて確認し、今回の未導入変更全体の効果とautosave単独の差を混同しない。

`save_candidate_write`／`save_candidate_write_ex`は名前末尾が`.ess`／`.ess.tmp`／`.ess.bak`の候補へのwrite syscall。自動／手動保存の識別ではなく、別命名の保存は一般writeに入る。`save_candidate_window`はそのhandleの最初に記録できたwriteからcloseまでの経過時間で、待機／複数thread／失敗したwriteも含み、保存全体のCPU時間・disk永続化完了ではない。windowのthreadは0とし、解析器も`handle_lifetime`と表示する。区間重複だけで因果を断定しない。

SPO比較は実ファイルの差分確認後に、対象が確認できた1室から始める。オートセーブ比較と同時にMODを変更しない。[作者の配布一覧](https://www.nexusmods.com/skyrim/mods/32505?tab=files)のログイン条件を回避せず、取得後も既存game／load orderを保つ。実差分と採否理由は[最新の性能記録](PERFORMANCE_WORK.md)を参照。


## 追加提案：ラグドール・死体処理・MOD管理

2026-11-14。友人の追加画像を受けた調査のみ。ゲーム設定・MOD・本番コードは変更せず、build／push／インストールも行わない。

- **Realistic Ragdolls and Force / Medium**：classic作者の説明ではpluginは力の強さを変更し、ragdoll／skeletonの変更とは別。吹き飛び方を選ぶ任意MODとして検討できるが、力を弱めるだけで物理計算時間・FPSが改善する証拠はない。元の物理挙動を保つ最適化には含めない。今回実ファイルの差分は未解析。
- **Dead Body Collision Remover**：指定名の正規配布元を特定できなかった。似た名前のDead Body Collision Fix（classic 30947）は作者説明上、死体とplayer／NPCの衝突を追加する逆方向のMOD。名前から削減対象・互換性・効果を推定せず、正確なURL／版が得られるまで保留する。一般論として衝突削減は処理量を減らす可能性があるが、接触・移動等の挙動も変わる。
- **iRemoveExcessDeadCount=15 / fRemoveExcessDeadTime=10.0**：Rootが固定所有入力のSkyrim.esm／Update.esmのGMSTだけを読取り、Skyrim.esmにそれぞれ15／10.0、Update.esmに当該overrideなしを確認した。提案値は基準データと同じ。runtimeの現在値まで実測したわけではない。[General]へのINI追記で上書きされることも未証明なので、その形では採用しない。将来の値変更は死体・戦利品の保持に関わる任意の挙動変更として扱う。
- **MOD管理**：独立した機能として有用。原Dataを保持したまま、MOD別保管・起動前ON/OFF・master依存／load order／競合表示・profileとsaveの対応管理を検討する。まずclassicの互換性を確認したデータMODを対象にする。Windows用native DLLは管理画面を付けても実行可能にはならない。「いつでも安全に外せる」やプレイ中のhot reloadは約束しない。今回は設計上の候補整理まで。

根拠：[Realistic Ragdolls and Force作者](https://www.nexusmods.com/skyrim/mods/601)、[Dead Body Collision Fix作者](https://www.nexusmods.com/skyrim/mods/30947)、[Mod Organizer 2のprofile実装](https://github.com/ModOrganizer2/modorganizer/blob/master/src/profile.cpp)。死体の数値は所有classicデータのローカル読取りによる。内部データやログは外部送信していない。

## 追加リンクと次のbuild範囲

[Dead NPC Body Cleaner Remover SE](https://www.nexusmods.com/skyrimspecialedition/mods/10614)は離れた後の死体・灰の除去を行うMODで、collisionだけの修正ではない。[classic版](https://www.nexusmods.com/skyrim/mods/84917)の作者要件はDawnguard/Hearthfire/DragonbornとSKSE。現在のbase+Update構成にはそのまま入らない。死体が蓄積した場面での効果候補と、camera/magic時のstallを区別する。

[Possessive Corpses](https://www.nexusmods.com/skyrim/mods/4528)はv1.8、最終更新2012-02-17。作者は武器・残骸・nirnroot等の蓄積とsave肥大化への対処を説明している。安定という友人の感想を1.9.32移植の検証結果とは扱わない。正規実packageと現在のscripts/recordsの重複・挙動差を確認するまで導入保留。Dead Body Collision Removerが削除済みという点も、今回のリンクでは確認できない。

ユーザーは「3までやったらビルドする？」「ではお願いします」で、①採用できる停止対策、②内部heapの安全な観測方法、③MOD管理の最小設計を整理した後の一括Mac buildを承認。①は派生MSL保持削減を採用、singleflightは保留。②は元の統計lockの無期限waitを発見したためpollを接続せず、既存writer境界へ接続する条件を保存。③は[設計書](MOD_MANAGEMENT_DESIGN.md)へ確定。次のbuildには検証済みhostと既存autosave切替を含めるが、heap増量・SPO等のMOD・MOD管理UIは含めない。
