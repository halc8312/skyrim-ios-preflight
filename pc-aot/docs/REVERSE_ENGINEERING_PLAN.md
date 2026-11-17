# Skyrim PC版の部分逆コンパイルとカクつき改善計画

## 2026-11-17：実生成物による優先順位の補正

添付scannerを9件のsynthetic検査後に固定入力へ実行し、build13で使う既存C／ARM64 objectまで照合した。x87と一部SIMD storeの分類漏れは実生成物でも存在する。ただし、残りのメモリ順序保護の解除・leaf一括native化の根拠にはならず、LIFT_TSO／MEMORDER／LB_STRICT_ORDERは維持する。詳細と静的件数の制約はPERFORMANCE_WORK.mdの同日記録。

次に読む対象は、imported InterlockedとSleepを使う実際の共通reader/writer同期群。所有者の再帰取得・28-bit reader count・条件付き取得と途中遷移があるため、取得／解放の一部だけをmutexへ置き換えない。原条件を真実としてwait／retryを計測し、frame停止と対応してから置換契約を詰める。scannerのゼロ書込み最上位は初期化用途が確認され、解放関数としての選定を取り消した。spin候補0はheuristicの検出漏れであり、同期ループ不在ではない。

CRC／copyの既存契約と有限比較は再利用する。static call数やleaf候補率をCPU寄与と同一視しない。guest側の保護設定、generic DXVK lock、dxvk-csのQoS、shader state生成の調整は別実験に保ち、同時変更による採否の混同を避ける。

## 追加レビュー：変換器改善を先行可能な独立段階に（2026-11-14）

[段階移行設計](NATIVE_MIGRATION.md)へClaudeの追加7提案と3試作の注意点を照合・反映した。手作業のleaf化に先立ち、既存のGPR local／direct call／間接call cacheを区別し、証明できるper-bit flags生存解析とx87 stack静的化を局所比較する。通常MSVC ABIの慣習だけで未知entryのflags／FP状態を捨てない。

D3D9のFPU設定未対応は既存端末ログでも確認したため、CreateDeviceのguest CWと実演算精度を数学leafの前提として追加。通知hintは元の状態確認を残しても要求世代・所有者寿命・APC処理の証明が必要。macroのtimeout後二重生成は待ち時間／熱の増加を招き得る。managed texture evictionは固定DXVKにあるが、readback／待ちとの交換条件が未測定なので有効化しない。今回は計画・記録だけでruntime／guestは変更していない。

## 追加3件と全面復元案の判断（2026-11-14）

下の①〜③をさらに進め、request constructor／destructor・別完了writerを元命令で追跡し、DDS payload viewと同時macro生成共有を独立試作した。自作比較はそれぞれ2,031条件、8同時生成→1回と通常／ASan/UBSanで成立したが、loaderのguest寿命／failure契約と実Metalのdeadline／推移的lockは未確定。両方`experiments/`に保持し、本番への接続は保留。通知待ちも全writer／要求世代を覆う証明前には有効化しない。詳しくは[性能記録](PERFORMANCE_WORK.md)冒頭。

全面逆コンパイルの提案は[段階移行設計](NATIVE_MIGRATION.md)へ反映。読めるソースの復元と、guest表現のコストを減らすARM64向け実装は別段階として検査する。現在もARM64機械語で実行しており、全エンジン復元前に狭いnative境界を導入できる。新GOまでアプリ／private guest／Apple build、push、署名、更新は保留。

## 2026-11-14：①〜③のgoalで確定した実装・設計

対象は①shader／素材準備、②textureの読込み・展開・転送、③読込み要求と完了待ち。以前の18関数に加え、21の入口を元命令で限定確認した。新しい報告は重複を含め22入口・1,031個の異なる命令位置で、同じtailを共有する入口を二つの独立実装とは数えていない。関数対応、擬似コード、TLS／所有権／callbackの詳細はRoot専用の`private/analysis/stutter-goal-20261114-01/CONTRACTS.md`に保存。ここには公開runtimeの判断と個人・ゲーム情報を含まない集計だけを記す。

| 対象 | 今回の結論 | 現時点の採否 |
| --- | --- | --- |
| ① shader／素材 | ゲーム側には既存検索とlazy作成がある。shaderペアが分かる時点でも最終PSOの全stateは揃わない。一方、公開DXVKの既知state先読みで同じ組合せの仕事が重複する | **先読みindexの重複排除を実装・ローカル検証済み**。ゲーム関数のworker移動や推測PSOは保留 |
| ② texture | guestのstream、mip選択／再コピー、hostのdecode／uploadは別段階。resource公開とTLS条件付きcallbackまで含む関数を一括移動できない | **保持するmipをviewで渡す設計まで完了**。実頻度と所有権の追加確認後に限定適用 |
| ③ I/O／待機 | 同一要求threadはAPCを処理する一方、別threadは最大10,000回Sleep(0)後にSleep(1)を繰り返す経路がある。現在のhost ReadFileExは同期pread後にAPCをqueueする | **通知待ちへの限定置換と本来の非同期I/Oを別設計として確定**。実頻度・全完了通知・寿命の未確認部分があるため本番置換しない |

### ① 既知pipeline先読みの重複を削る

固定DXVKの読込時indexは、同じshader組合せにN個のstateがあると同じ組合せをN回登録し、最後のshaderが揃うとN個のjobを投入する。各jobが全N stateを辿るため、N²回のstate訪問になる。実際のpipeline生成には既存cacheがあり、N²回のMetal compileが発生するという意味ではない。

`prepare_dxvk.py`で、全state／format／entry IDを維持しながら、完全一致するshader組合せのwakeup indexだけ一度登録する。既存keyのhashと全stageの等価判定を使い、追加setや描画中のlockを作らない。cacheファイル形式、reader／writer、shader登録処理、worker内のcompile処理は維持。読込みはworker開始前に完了する。

自作6組合せ×16 stateでは、必要な96 stateを全て処理したまま、wakeup link **208→13**、job **96→6**、state訪問 **1,536→96**。旧新の実メソッドと実keyを使い、1／2／16／128 state、順序反転、shader共有、全graphics stage、compute、部分登録、並行登録、失敗対象を比較した。通常／ASan/UBSanと実ヘッダー付きDXVK translation unitのsyntax検査を通過。GPU compilerとcacheレコードの供給部分は自作fixtureであり、iPhoneの速度測定ではない。[性能記録](PERFORMANCE_WORK.md)に範囲を記載。

起動時に一行だけ`IOS_STATE_CACHE entries=... shader_tuples=... wakeup_links=...`を記録し、実データの重複規模を確認できる。これは投入済みjob数やcache hit数ではない。繰返しregisterShaderそのものの排除、未見stateの先読み、初回Metal compileの消去はしていない。

追加の早期PSO準備は、元shader bytecodeの借用ではなく、保持済みhost shader・device世代と完全なstate／render pass keyが揃ってからにする。ゲームがbytecodeを解放する順、TLS、include callbackをworkerへ持ち出さない。既存先読みと重複する仕組みを追加せず、未完成なら現行の正しい待機を維持する。

### ② mip再コピーを省く最小設計

現hostは、形式・寸法・color key条件が合うDDSを直接uploadし、必要な場合だけpixel変換する。すべてのtextureをRGBAへ展開しているわけではない。既存cube layout／point変換の削減を再実装せず、悪化した一括行コピーやBC2/BC3案も戻さない。

次候補は、選択後のDDS headerと元payloadを一つの論理viewとしてhostへ渡し、**保持するpayloadの再コピー1回**を省くもの。単にpointerをずらすとDDS headerがなくなるため成立しない。導入条件は以下。

1. 読込み・image-info確認・元のmip policy判定は同じthreadで実行し、一度決まった選択を保存する。適用は完全に範囲確認できる2D圧縮DDSに限定。cube／volume、未確認layout、overflow、切れた入力は副作用前に元経路へ戻す。
2. 調整済みheaderとread-only payload spanを明示する内部host APIを定義する。元bufferを書き換えず、format／alpha／pitch／mip／寸法／usageを保つ。元のskip計算と一般的なDDS計算が一致しない奇数寸法等を勝手に補正しない。
3. 同期consumer終了まで元bufferを保持し、非同期uploadには独立したstaging所有権を渡す。guest pointerをreturn後まで借用しない。allocator／free／公開callbackの観測可能な副作用を確認してから、その部分だけを置換する。途中失敗後に元処理を再実行しない。
4. 自作DDSで、全保持mipのbyte範囲、descriptor、padding、失敗とrelease順を比較する。小さなmip、skip境界、極端なpolicy、truncation、末尾データ、Create／Lock／Unlock失敗も含める。Appleの最終pixel比較は新GO後。

採用前の計測はguest stream／archive展開、mip-copy bytesと時間、host decode／Lock・upload／resource waitの区別。既存`texture_load`はguest側のstreamやmip-copyを含まない。呼出頻度やコピー量が少ない場合は、interfaceと寿命管理を複雑にする価値がないので棄却する。今回、private guestは変更していない。

### ③ 完了待ちを先に絞り、非同期I/Oを別扱いにする

**W1：別threadのyield loopだけを通知待ちへ。** まず待機mode、要求threadとの一致、elapsed、yield回数を固定量の数値で測る。頻繁なら、要求の世代を持つhost wait recordを作り、実guest callbackがデータと完了flagを公開した後に通知する。通知だけで成功扱いせず、前後でflagを再確認する。全state writer、request再使用、破棄・取消・callback再入まで被覆できた場合にだけ有効化する。同一threadのAPC処理と非blocking分岐は残す。現行のforeign-thread非blocking分岐には独自の戻り方があるため、汎用condition waitへ一括置換しない。

**I1：ReadFileExの非同期化。** I/O workerを足すだけでは不十分。受理前にfd leaseと要求threadの世代を取得し、CloseHandle／handle再利用と同期する。workerにCPU*、解放後のGFile*、guest callbackを渡さない。状態は「予約→受理→I/O→完了queue→要求threadのAPC→退役」。受理前の拒否はI/Oもcallbackもなく、受理後の結果不明時に同期readを重ねない。

完了queueとalertable waitは同じpredicate／通知規約を使い、空確認とsleepの間の通知を失わない。buffer・OVERLAPPEDは必要期間保持し、取消後もOSの書込みが終わるまで解放しない。要求thread終了／アドレス再利用、queue上限、shutdown、WriteFileExとの順序も契約化する。worker数ではなく「未通知の完了も含む全受理要求数」を制限する。

現在のSleepExのINFINITE→1ms処理と、APCを処理しないWaitForMultipleObjectsExをそのままにして、本来の非同期readだけを入れてはいけない。Microsoftの[ReadFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfileex)、[SleepEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepex)、[WaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjectsex)の要求thread・alertable completion・buffer寿命の契約を照合した。ReadFileExのhEventはアプリ独自用途に使えるので、一般のReadFileと同じevent通知を追加しない。

最小比較は、通知がwait登録の前／後、要求／別thread、blocking／非blocking／nonalertable、0／有限／無限待機、複数／再入APC、partial／EOF／失敗、64bit offset、fd位置、close/reopen、世代再利用、queue満杯、取消／終了。時間短縮だけでbyte・error・callback順の不一致を許さない。現時点ではこの大きな互換変更を採用する実機根拠が不足している。

### 次回GO後の確認と判断

回収済み約17分の部分ログでは、pipeline生成は最大約572ms、100ms超220件。一方、ReadFileEx syscallは最大約42ms、通常readは約27ms、D3D shader作成は約1ms。texture loadは約131msが1件あった。別thread・入れ子・ロード画面を含む集計なので、これらを足したり、I/Oが常に軽いと結論したりしない。

まず既存の未導入対策と今回の①を、許可された一度のApple buildへまとめる。現在の端末版／元save／旧IPAは保持する。同じsave・画質・FPS capで、静止→camera→攻撃／魔法→同場面の再訪を比較し、起動間cache比較はホームへ戻って保存機会を作ってから行う。測るのは50／100ms超のframe gap、同thread／時刻に重なる生成・待機区間、cacheの組合せ数、温度推移。既存の固定stall記録には容量上限・取りこぼしがあるため、記録なしを発生なしとは扱わない。

①は実際の余分な先読み処理が減るかを確認し、初見shaderが残ることを前提にする。②は実際のcopy負荷、③は別thread待機／I/O依存が観測されてから限定置換へ進む。これは3系統のローカル調査・採否・設計の区切りであり、全カクつき解消や3系統すべての本番置換完了ではない。新しいアプリ／private guest／Apple／cloud build、push、署名、端末更新はGO待ちを維持する。

## 2026-11-14：初回生成待ちの追加調査と外部レビューの論点

本人がPC版Claudeから送信し、Fable 5.2／高の実画面と完了した初回回答を確認した。公開MoltenVK sourceと照合した採否は[性能記録](PERFORMANCE_WORK.md)のClaudeレビュー欄へ保存。固定DXVKのURL、集計値では因果未確定という訂正、休止gateのCPU semaphore反例を追加で渡し、2回目の回答も完了表示まで確認し、既存対策で覆う指摘と未確認経路を性能記録へ追記した。次は本人の指示によりエンジン全体の分析をまとめて相談する。画面取得／送信操作の不調は残るが、追加文も本人の操作で送信済みと確認できた。公開ソース・自作fixture・匿名集計だけを共有し、私有shader／game code／生ログ／署名情報は送っていない。GO待ちは継続。

### 次の包括分析（本人の追加依頼）

Fable 5.2／高への包括分析依頼が送信され、完了した回答も取得済み。初回の画面は応答中のままだったが、再読み込み後に完了を確認し、全文を読んだ。以下へ候補とRootの留保を保存。まだ全候補の独立検証や実装を終えた状態ではない。

対象は固定Skydreamの公開recompiler／runtime／DXVK、固定MoltenVK、自作iOS hostの説明、既存の匿名数値。原ゲームや生成関数、shader、raw log、署名情報は渡さない。Claudeへは実装・build・PC内やコネクタへのアクセスを依頼しない。

- CPU/AOT：flags、ABI、spill、dispatch、alias、x87/SSE、memory ordering。既に命令同一や悪化で棄却した案を繰り返さない。
- thread：main／guest／CS／submit／compiler／audio／I/Oの依存、優先度逆転、CPU起床。コア固定やworker増加を目的にしない。
- 描画：state/binding、descriptor、pass、resource寿命、staging/copy、shader初回準備。直接Metal化と現行層の改善を比較する。
- streaming/memory：read、展開、mip/copy/upload、allocator、cache、page fault。ReadFileEx非同期化は要求thread通知・buffer/fd寿命・取消まで条件化。
- engine：物理、animation、visibility、scene updateの限定逆コンパイル候補。負荷が未観測なら保留し、ゲーム更新や描画の省略はしない。
- 音声／energy：展開、mix/queue、CPU起床、継続負荷。画質・FPS設定は利用者が選び、内部最適化と分離する。

求めた出力は、確認事実／推論／未計測を分けた経路図、優先候補8件以内（固定source根拠・削減対象・互換条件・最小検証・棄却条件）、逆コンパイル候補の最初3系統、端末なしで進める3件と実機で測る3件、native Metalへ段階置換する場合の最小境界。関連移植・互換layerの情報も一次source、版、実行方式、licenseへ遡るよう指定。根拠のないFPS改善率や「全面再構築すれば速い」という推測は求めない。

### 包括分析で得た候補とRootの留保

Claudeの暫定8候補は、①CPU状態とguest memoryの非alias情報／MEM参照、②x87 stackの静的割当、③呼出境界の退避・復元の削減、④strict-order命令選択と適用範囲の監査、⑤待機・起床・優先度、⑥drawごとのdescriptor経路、⑦Lock/Unlockのshadow copy／texture展開、⑧I/Oと音声decodeの分散。部分逆コンパイルの最初の3系統はshader／描画状態の準備、texture streaming、job／待機loop。物理／animation／visibilityは実負荷が上位と確認できるまで後回しとする案だった。

Rootは固定公開sourceのSPILL/RELOAD、MEM global、ST macro、lifterの呼出境界を確認した。ただし次の補正が必要：

- C上の退避式の数は、Apple compiler後のARM load/store数や時間ではない。「1呼出32回」「MEMは必ず毎store再load」「restrictで必ず関数1回になる」は実機械語を確認するまで性能事実にしない。現portは`0005-call-flags.patch`でFlagsを呼出境界に保持しており、公開snapshotだけの解析ではこの変更を見落とす。フラグを含む既存のCPU ABIを保持する。
- CPU*のrestrict指定は、単にguest領域と確保元が違うだけでは不十分。別名pointer、TLS、callback／再入、setjmp/longjmp、host override、診断の観測点を追って初めて成立条件を判断する。誤ったrestrictは未定義動作になるため未採用。
- 退避削減で「診断が古いregisterを読んでもよい」という提案は採らない。既存の可観測なCPU状態を保持し、未知のcall／import／間接分岐では保守的な経路を残す。命令単体のdifftestだけでは呼出／例外／並行状態の同等性を証明しない。
- RCpc/TSOに関する外部の議論や他projectの数字は、このportへの命令置換の許可ではない。現atomic/fenceを維持し、公開一次資料・現ARM出力・対象機能・メモリモデル全体を別途確認する。未被覆の浮動小数／vectorアクセスも正しさの監査事項で、今は一括変更しない。
- main以外でI/O／decodeしているだけでは影響がないとは限らない。描画側の待ち、lock、CPU競合、callback依存を含める。専用threadや同時要求数1だけを理由に自動棄却しない。
- descriptor再利用にはbinding内容だけでなく、layout、dynamic offset、resource世代／寿命、pool resetとGPU退役が必要。現pool事前判定と別の改善候補として扱い、argument-buffer設定を勝手に変更しない。

次の判断は、既存のARM生成物で不要load/storeが残るかの限定監査、呼出境界／aliasの契約整理、待機・I/Oの既存対策との差分整理から行う。新しい実ゲームのcompileや全関数逆コンパイルは開始しない。実機ではCPU sampling、停止とpipeline各段階の時刻対応、I/O／texture／shadow copy量を優先する。native Metal全面再構築は初回compileが残るうえ互換範囲が大きく、現段階で高速化を保証する根拠がない。狭いhost経路から比較する。

Claudeが挙げた関連資料は[固定Skydream](https://github.com/meizfl/skydream/tree/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a)、[FEX x87解説](https://fex-emu.com/FEX-2408/)、[Lasagne論文](https://doi.org/10.1145/3519939.3523719)、[Arm RCpc解説](https://community.arm.com/arm-community-blogs/b/tools-software-ides-blog/posts/enabling-rcpc-in-gcc-and-llvm)、XenonRecomp／Box64／DXMT等。外部の性能数字・適用可能性はRootによる一次資料の再検証前であり、採用根拠として確定していない。GTA移植等の二次情報や公開実装のない推測は根拠にしない。

### Gemini 4 Argonへの独立レビュー

本人の指定アカウントの既存sessionで、Gemini 4 Argon／高を選択し、描画の優先3仮説について完了した回答を取得した。公開固定URLを含む長文依頼は回答本文を取得できなかったため、検索なし・匿名集計と自作説明だけの短い質問へ分割した。従って、取得済み回答は提示情報に基づく仮説レビューであり、Geminiがリポジトリや一次文献を独立に監査した証拠ではない。原ゲーム／生成関数／shader／raw log／save／framework／署名情報は未送信。初回AI Studio設定は使わず、Geminiの既存利用環境で進めた。

| Argonの優先仮説 | 最小限の切り分け | Rootの採否と補正 |
| --- | --- | --- |
| MoltenVK〜Metalの最終PSO作成と下位lock待ち | 同一要求のDXVK lock、MoltenVK queue／monitor、Metal API開始〜callback、呼出側再開を匿名threadと時刻で分ける | 調査候補として採用。既存の圧縮MSL保存だけで最終PSO再利用とは言えない。binary archiveを直ちに最小修正とは認定せず、function特殊化の重複とPSO生成の内訳から選ぶ |
| guest mainとUIKit mainの同居、RunLoop／drawable待ち | 長いframe gapをRunLoop、既存Present段階、drawable取得と同じ時刻軸で対応させる | 未確定。guestの別thread移動はUIKit／SDL、TLS、APC要求thread、callbackと寿命の監査が必要な大変更。drawable取得の遅延もearly enqueueとswapchain順序を維持する必要があり、無条件に採用しない |
| 1frame内の複数PSO待ちの集中 | frame・要求IDと実際の依存関係を結び付け、描画を進められなかった区間を測る | 調査候補として採用。複数threadや入れ子の時間を単純合算しない。D3D9のshader作成時にはvertex declaration、RT／depth format、sample、blend等が未確定の場合がある。全stateを知る前の先行生成やD3D11型のstate objectを仮定しない |

最大571.642msのpipeline生成、368.061msのlock待ち、2,124.922msのPresentは、同一時刻・要求との対応が未取得。Argonの説明にこれらを結ぶ仮説はあるが、同じ停止の内訳や主因とは確定しない。既存DXVK state cacheは必要shaderが揃った既知variantを既に先行準備するため、同機能の重複実装を避ける。初回回答への補正とCPU監査をまとめた追加質問は送信後に会話から消えて本文を取得できず、CPU側だけの短い質問も生成表示後に会話から消え、追加回答は取得できなかった。取得できた描画の1回答だけを成果とし、CPU提案をArgonの回答として補完しない。

Rootの固定source確認：MoltenVK `MVKSync.mm:570`の`MVKMetalCompiler::compile`は`dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0)`へblockを送り、呼出側は`_blocker.wait_for`で結果または既定timeoutを待つ。Metal APIがcompletionHandler形式であることは、Vulkan呼出側のstall解消を意味しない。DXVK workerのQoSだけで、このGCD処理やdriver内部compilerの実効QoSが同じになるとは確認していない。実queue待ち／device monitor待ち／Metal API入口からcallback／呼出側の再開を区別し、QoS継承・override・依存を計測してから判断する。default queueの使用だけで優先度逆転を断定せず、一律に低優先度へ変更しない。late callbackと`destroy`の既存寿命を保持し、timeout変更や新しい待ちを追加しない。

ClaudeとArgonの共通点は、圧縮MSLより下の生成処理、待ちの依存関係、既知入力の再利用を優先して調べること。これは新しい性能実測ではない。次の判断順は、既存の細分化計測と未導入候補を維持し、function再取得／特殊化の重複、最終PSO再生成、既知variantの準備時間の順に削減可能性を確認する。重複がほぼない、driver内部cacheで十分、余分なretain／lock／memoryで悪化する場合はfunction再利用案を棄却。archiveは機器／OS／source世代、容量上限、失敗時の既存compileを必須とし、未見shaderの初回生成を消すと約束しない。CPU・エンジン本体の置換は上記の包括分析と実負荷の確認条件を維持する。この追記では実装・Apple build・実機検証を行っていない。

### 今回ソースで確認した境界

- 固定MoltenVK `458870543b9bcf6b0edd6f90aaa776707b310d96`の`MVKPipelineCache::writeData`はshader key、変換設定、変換結果、圧縮MSLを保存する。最終`MTLRenderPipelineState`を保存する経路ではない。従って約12.27MBの保存成功だけで、次回のMetal compileや全PSO生成がなくなるとは言えない。
- 固定DXVKの`DxvkStateCache::registerShader`は、登録されたshaderに対応する既存pipelineを列挙し、必要な全shaderがmapにある場合だけworkerへ送る。`compilePipelines`が既存state variantを準備する。ゲームがshaderを初使用直前まで登録しない場合、既存先読みにはその分の時間的余裕がない。未知のstateはcacheから予測できない。
- MoltenVKの`MVKShaderLibrary::getMTLFunction`はlibrary取得後にもfunction取得／定数特殊化を実行し、別にrender／compute pipeline compilerがMetal PSOを生成する。既存の`shader_module`計測はこの内訳ではない。graphics pipeline全体の最大約572msを、そのままMSL変換だけの時間に読み替えない。

### Claudeへ依頼する独立レビュー

1. 上記境界とbuild5の匿名集計（約1,043秒、graphics pipeline 1,867件・平均24.924ms・最大571.642ms、compile lock待ち最大368.061ms、100ms超frame gap 122件）から、次の一手を順位付けする。複数thread・ロード／メニュー込み・低頻度sampleのため因果未確定であることを前提とする。
2. 未導入のdescriptor容量切替、準備済みcache lookupのlock分離、圧縮payload copy 3→1、metadata copy削減について、正しさ・寿命・失敗処理の反例を探す。局所fixtureでの削減を実機FPS改善とみなさない。
3. 初回生成には、端末内Metal binary archive、限定したMTLFunction再利用、必要入力が揃った時点の先行準備を比較する。前二者は未見shaderの初回生成を必ず消す方法ではない。正確なdescriptor／constant値、device／OS／source世代、bounded memory、取消・失敗fallback・warm/cold比較を具体化する。未完成drawの省略、精度低下、shaderの外部送信は不可。
4. `3333cc7`のGPU lifecycle gateを別件として厳しくレビューする。early enqueue、64slot満杯、同期callback、CPU側emulated semaphoreの待ち、別queueからのsignal、main-thread lifecycleとworkerの依存、scheduled待ちのwatchdogリスクを含める。通常playのカクつき対策とbackground送信拒否の対策を混同しない。実Metal／UIKitで未確認の部分を明示し、反例があれば自作fixtureで再現してから修正する。
5. 提案ごとに、公開一次ソースの根拠、変える最小単位、反例、必要な実機測定を求める。単純なworker増加やdevice全体lockの削除を根拠なく採らない。

Appleの[Metal binary archives](https://developer.apple.com/documentation/metal/metal-binary-archives)と[Metal 4 compilation API](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api)も再確認。端末向けbinary再利用・段階導入は候補だが、今回の固定MoltenVKへの実装や性能改善の証明ではない。相談後も公開sourceと実fixtureで再検討してから採否を決める。

**2026-11-14追記：** 明示的なgoまでアプリ／クラウドビルドしない。公開hostで同一shaderの同時コンパイル共有とAPC通知queueのring化を追加し、通常／ASan/UBSanと実source syntax検査を通過した。[最新の数値と残課題](PERFORMANCE_WORK.md)を参照。ゲーム本体の関数置換・I/O非同期化・初回shader全般の先読みは未実装。`d3d_shader_wait`で次回の実機に同一key待ちがあるかを測り、既存のread／HLSL／pipeline／resource待ちと区別する。

**build5試遊後の更新：** 本人から60 FPS到達と発熱軽減が報告されたが、負荷／視点変更時の停止と水中の茶色表示は残った。Rootが水関連48shaderをPC内だけで解析し、既存float互換modeのDXSO→SPIR-V→MSL変換を確認した。実水中passの同定・Apple実行・画素の修正確認はまだない。詳細は`private/analysis/water-20261114/README.md`、公開できる数値と候補は[性能記録](PERFORMANCE_WORK.md)へ保存。

次回候補には50ms以上のframe gapとhost処理の固定sample、同時刻／匿名threadを比較するread-only解析CLIを追加した。先にframe gapとshader／read／resource wait等の重なりを確認し、その上位経路に限って本体関数の逆コンパイル・置換を進める。Overlapは因果証明ではなく、低頻度sampleにない経路も除外できない。古いbuild4を次の試験版とする下記表現は履歴であり、現在の比較基準は実機build5。新しいgoまでbuildは行わない。

**11月14日朝のshader調査。** 初回MSL compileとfunction specializationは残る。[PPSSPPの計測](https://github.com/hrydgard/ppsspp/blob/master/docs/shader-compilation.md)と[DXMT向けの公開パッチ](https://github.com/tholtman1-del/overwatch-mac-stutter-fix/tree/main/patches)を固定経路と比較した。後者のdraw省略は表示の遅れを許容する手法で、今回の品質維持条件には採らない。compilerの数／優先度制限は現hostに既にあり、通常6CPUならDXVK worker3件、job境界でUtility／Backgroundへ切替える。D3D11 compute shader作成時の先行生成はD3D9 SM3へ直接移植する入口がないため、同名の設定を増やさない。

[AppleのMetal 4 compilation API](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api)にはcompilerのQoS、非同期実行、pipeline収集／再利用がある。[binary archive](https://developer.apple.com/documentation/metal/metal-binary-archives)はGPU向けbinaryを保持できるが、固定MoltenVKの圧縮MSL cacheを自動的に置換する設定ではない。将来の候補として、実pipeline descriptorとfunction constantsを完全に捕捉し、端末／OS／source世代で区別した端末内archiveを段階導入する。失敗／miss時は元のcompileを維持し、未完成drawを省略しない。ゲーム由来shaderやarchiveをクラウドへ送らない。Metal 4非対応端末／OSには既存経路を残す。これは設計候補で、実装・実測済みではない。

もう一段小さい候補は同一library・同一function constantsの`MTLFunction`再利用。現在はpipeline作成ごとにfunction取得／特殊化を行うが、Metal内部cacheの効果は未測定。導入前に実際の重複数を確認し、macro variant、entry point、定数の型／未指定値、workgroup寸法、所有権、error／feedback、上限と退役をキー／寿命規則へ含める。単にVkSpecializationInfoのpointerをキーにする実装や、device lockの無条件削除は行わない。

2026年11月13日。目的は、平均FPSの向上に加え、操作中に画面が長く止まる回数を減らすこと。既に動いている静的再コンパイル版を基準とし、時間を使っている関数を少数ずつ読み解き、同じ結果を出すARM64向けの実装へ置き換える。

端末なしでできる最初の解析を実施し、3系統の候補と置換条件まで整理した。ゲーム関数の本番置換と新Macビルドはまだ行っていない。サブエージェントと定期自動送信は使用しない。

## 現在地と今回の範囲

- PC版build2は本人の実機報告でゲームプレイに到達。90,390関数の静的変換が基盤であり、全関数の意味を復元して最適化した状態ではない。
- build4候補には、書込みhelperの命令削減、テクスチャ管理の検索縮小、pipeline cacheの永続化、固定時間分布、準備済みpipelineを長いコンパイル待ちから分離する修正が入っている。ローカル検証済みで、実機の改善量はこれから確認する。
- 既存の文字列4関数の32,000比較、メモリ転送とCRCの20,000比較を再利用する。ただし全CPU flags、SIMD状態、並行処理、ARM64までの同等性を証明した結果ではない。これらをそのまま本番置換の承認には使わない。
- 元exe、生成コード、private framework、セーブ、旧IPAは保持する。逆コンパイル出力とゲームに対応する呼出元情報はPC内の`private/`へ置く。リポジトリへ反映するのは公開ホスト、自作fixture、ゲーム内容を含まない計画と数値だけ。

11月13日時点の基準は既存build4候補だった。その後の追加変更は未ビルドで保持し、最新の本人指示に従ってgoを待つ。以前の20時以降という予定は実行許可ではない。逆コンパイル候補が未検証のまま混ざらないようにする。

## 調べる順序

| 優先 | 対象 | 調べたいこと | 次へ進む条件 |
| --- | --- | --- | --- |
| 1 | ゲーム側のシェーダーと素材の準備 | 初使用時のコンパイル要求、同じ要求の繰返し、準備を行うタイミング | 停止と対応する呼出経路や重複処理を確認できる |
| 2 | メモリ転送と文字列とCRC | 既に解析した関数が実際に多くの時間を占めるか | 呼出頻度と合計時間に改善余地があり、完全な入出力条件を整理できる |
| 3 | 物理とアニメーションの更新 | 継続的なCPU負荷と更新処理の偏り | 上位の負荷として観測され、関数単位の同等性を比較できる |

この順序は初期の調査順。実機で主因がGPU処理やデータ読戻しと分かれば、その処理へ優先順位を移す。特定のエンジン関数が重いと決めつけて逆コンパイルを広げない。

## 1 端末なしで呼出経路を絞る

所有入力の既存discoveryと生成Cを使い、公開API境界から呼出元へ戻って調べる。最初の入口は`D3DXCompileShader`、D3D9のshader作成、texture作成とLock、ファイル読込み。既存の逆アセンブル結果とCを照合し、必要な関数だけ追加の逆コンパイル対象にする。

一度に扱うのは、時間がかかる可能性のある入口とその直接の呼出元を中心に最大3候補。間接呼出しやobjectの意味が不明なら、その不明点を残す。名前だけを根拠に意味を確定しない。ホストの戻りアドレスとゲストの呼出元を同じものとして扱わない。

各候補では次の内容を短く記録する。

- 入力、戻り値、書込み先、参照するglobalとTLS。
- 呼出順、ロック、実行thread、失敗時の副作用、再入の有無。
- 同じ入力で繰り返す処理と、その間に変わり得るデータ。
- 既存APIへ置き換えられる範囲と、ゲームの状態に依存して残す範囲。

成果物はPC内の候補一覧と読みやすくした擬似コード。今夜の実機結果が来る前でも、この静的な切り分けは進められる。重い処理の断定と本番置換の選定は、次の計測結果と結び付ける。

### 最初の解析結果

90,390関数の既存生成Cから直接参照を索引化し、候補周辺の18関数・2,125命令を元PEで照合した。索引は間接呼出しと未生成候補を網羅しない。シェーダーimportへの4つの直接呼出箇所は元の命令境界でも確認した。住所・擬似コード・依存globalなどの詳細はPC内の`private/analysis/stutter-paths-20261113-01/CANDIDATES.md`に保存している。

| 候補 | 確認できたこと | 置換・改善へ進む条件 |
| --- | --- | --- |
| シェーダー準備 | 一部にはキー検索と作成済みshaderの再利用が既にある。HLSLを通さず、bytecodeから初使用時にD3D shaderを作る別経路もある | HLSL・D3D作成・pipeline作成のどこが長いかを分ける。先行準備ではbytecodeの所有コピー、device世代、元の公開順を維持する |
| テクスチャの準備 | stream全体の読込み、条件付きmip選択とコピー、D3DXのdecode/uploadという段階がある | 長い段階を特定する。copyを省くなら選択mip・format・pitch・元bufferの寿命を維持する |
| 非同期ファイル読込み | 現shimは`ReadFileEx`内で`pread`を完了させてからAPCを予約する。callbackは後の`SleepEx`で呼ぶ | 呼出元の待ち時間が実際に大きいことを測る。workerへ移す前に同じ要求threadへの通知、fdとbuffer寿命、終了・取消・再入を設計する |

HLSLの一部にはTLS・共有global・include callbackの副作用がある。直接の呼出元を見つけられなかった関数もあり、「ソースがあるから現在のgameplayで重い」とは扱わない。素材読み込みがゲームのworker上なら、その待ち時間がそのまま描画threadの停止になるとも限らない。

次の一括build4へ、固定histogramの`read_file`／`read_file_ex`／`d3d_shader_create`／`texture_load`を追加した。元APIの引数・順序・戻り値とAPC処理は維持し、ゲーム関数や非同期I/Oの実行方式は変更していない。計測はshader名・path・guest address・素材を記録しない。これらは重なるhost処理時間なので合算せず、既存のframe間隔と合わせて候補を選ぶ。詳細な呼出元ringは引き続き必要時だけ追加する。

API契約の確認：[MicrosoftのReadFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfileex)では、完了までbuffer／OVERLAPPEDを有効に保ち、要求threadがalertable waitに入った時にcallbackを届ける。[D3DXCompileShader](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxcompileshader)ではmacro、include、entry、profile、flagsと複数の出力を扱う。本文だけのcacheや、guest callbackの無条件なworker実行ではこの契約を保てない。

## 2 実機で停止の種類を判定する

build4で同じセーブと画質を使い、静止、カメラ操作、同じ攻撃や魔法の初回と繰返し、短い移動を確認する。最初は一続きの短い確認と、ホームへ戻ってcache保存後の再起動を1回。原因が曖昧な部分だけ追加比較する。充電状態、熱状態、FPS上限、場面、起動後の経過時間を揃えるか記録する。

評価の中心は100ms超と250ms超のframe間隔の回数、最長停止、時間分布の上位側。平均FPSだけで採否を決めない。既存histogramから得るpercentileは範囲や上限として扱い、正確なp95やp99の実測値に見せない。

| 観測 | 調査の向き |
| --- | --- |
| 初回だけ長く、HLSLやpipeline作成時間も増える | 準備のタイミングと同じshader要求の重複を調べる |
| 繰返し操作でもcompiler待機が増える | 呼出threadとvariantを絞り、必要な生成と競合待ちを分ける |
| shader関連の増加がなくても停止する | ファイル読込み、展開、resource Lock、ゲーム側CPU処理を調べる |
| 時間経過や熱状態とともに悪化する | CPU起床、GPU負荷、FPS上限を含めて比較する |

5秒単位の同時増加だけでは因果関係を確定できない。特定が足りない場合は、関係するAPI境界だけに時刻、匿名の呼出元ID、処理種別、所要時間を記録する固定長ringを追加する。毎関数のログ、shader本文、ゲーム素材、毎frameのファイル書込みは避ける。容量上限と欠落件数を明示し、計測ONとOFFの比較で観測負荷を確認する。この詳細記録は今後の実装候補で、現在のbuild4には含まれない。

GPU実行時間は現在のhost時間計測から推定しない。必要になった場合は端末で利用できる計測手段を確認してから追加する。

## 3 最初の置換を設計する

最初は一つの原因、一つの限定された処理を選ぶ。元の静的変換関数を残し、起動時の設定で旧実装と候補を比較できるようにする。入力条件に合わない場合は関数の入口で元処理へ戻す。候補が副作用を起こした後に元処理を再実行する方式は使わない。

### シェーダーと素材の準備

重複があれば、入力が本当に同一と確認できる範囲から結果の再利用を検討する。shader本文だけでなく、include、macro、設定、compilerの版など、結果を変える条件を整理する。include callbackや素材ロードにゲーム側の副作用がある場合、それを省略するcacheは作らない。

準備を早められる場合は、ロード中など既に必要な入力が揃う時点を候補にする。ゲーム関数を無条件に別threadで呼ぶのではなく、寿命が確定したhost側の記述を作業単位にする。先読みの量とメモリを制限し、初期化順、TLS、復帰と取消を設計する。未完成shaderで描画を省く方法は採用しない。

### メモリ転送と文字列とCRC

既存の比較を土台に、実際に使うサイズ、重なり、alignment、境界、CRCの初期値や継続計算を確認する。CPU flags、SIMD、引数領域、戻り値と書込み、メモリ順序を含め、呼出元が観測する状態を明確にする。

ARM64の標準ライブラリーやSIMDで同じ処理を表現できる部分を候補にする。ABI変換と状態保存のコストを含めて測定する。既に同じ機械語になっていた固定長コピー案は、入力条件など新しい根拠がない限り繰り返さない。

### 物理とアニメーション

小さな更新スケジューラなど閉じた範囲から始める。既存のmotion関数には多数の呼出し、TLS、動的dispatchがあるため、一括置換しない。浮動小数点、時間刻み、globalの更新順、object寿命を復元し、衝突や移動結果を変えない。単に更新回数を減らす案とは区別する。

## 4 必要な比較を通して採否を決める

比較は変更箇所に対応する正常系、境界、既知の例外、実際に共有する状態へ絞る。既存の検証結果を再利用し、同じ検査を回数のためだけに繰り返さない。

1. 元処理と候補で戻り値、書込み、副作用、必要なCPU状態を比較する。参照も同じ変換器由来なら共通の誤りを持ち得るため、自作の独立した計算結果や命令検査を組み合わせる。
2. 並行処理や寿命を変えた箇所だけ、その競合と取消を検証する。
3. ARM64の実機械語と、変換・状態保存を含む局所コストを確認する。サイズ縮小やコンパイル成功をFPS改善に読み替えない。
4. 採用候補をまとめてMacビルドし、同じ場面で旧実装と候補を比較する。可能なら同一アプリ内の切替で比較し、方式ごとにクラウドビルドしない。

**採用条件**は、必要な動作の一致を保ち、狙った停止回数や待機時間が下がり、通常時の処理や熱状態が悪化しないこと。効果が計測のばらつきに埋もれる、又は悪化する場合は本番では有効にしない。cacheの初回と再利用後は別条件として評価する。

既存build4の複数変更をまとめた比較は全体の効果として扱う。個々の関数置換の改善率をそこから割り当てない。

## 着手順と区切り

| 段階 | 今できること | 完了の目印 |
| --- | --- | --- |
| 端末がない間 | シェーダー準備の直接呼出元、既存primitive、必要な状態の整理 | 最大3候補の役割と未解決点が分かる |
| 今夜の更新 | 既存build4を一括ビルドして署名し、接続後に更新 | 同じ場面の診断を回収できる |
| 最初の置換 | 計測で選んだ1候補を設計し、PC内で比較 | 動作が一致し、処理量削減の根拠がある |
| 次の実機比較 | 検証済み候補をまとめて評価 | 採用又は棄却の理由を数値で説明できる |

端末なしの解析と今夜の基準版準備は進められる。意味の復元や効果が未確定な関数を、今夜までに置き換えるとは約束しない。最初の一つで効果を確認できたら、同じ方法を次の負荷上位へ広げる。

参照：現在の実装と検証は[性能改善記録](PERFORMANCE_WORK.md)、既存の限定比較とその限界は[変換と関数比較](TRANSLATION.md)、端末とビルドの状態は[開発状況](STATUS.md)。
