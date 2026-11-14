# 段階的なソース化・ネイティブ化

2026-11-14。ユーザーが提示したClaudeの全面逆コンパイル案を、現構成と一次資料へ照合した設計。新しいアプリbuild・private guest生成・push・署名・端末更新の許可ではない。

## 結論と現在の構成

**動く静的再コンパイル版を残し、計測で重い部分から読める実装へ置き換える。全面復元を先行条件にしない。** 最終的に置換領域を広げることは可能だが、全エンジンの復元期間・互換性・速度はまだ保証できない。AIを使うことと、解析・測定・検証能力が無限になることは同義ではない。

現guestはx86→C→ARM64の事前コンパイルで、CPUが実行するものは既にARM64機械語。残るguestアドレス表現、flags／x87の意味、メモリ順序、Win32互換処理などのコストを、正しい境界で減らすのが次のnative化である。公開hostには既にC/C++/Objective-Cのnative実装があり、描画はD3D9→固定DXVK→固定MoltenVK→Metal。すべてを同じ方式で再実装する必要はない。

再コンパイル版は重要な比較対象だが、元PC版の唯一無二の正解と同一視しない。互換層や変換器にある既存の誤りまで新実装へ写さないため、元命令の契約、独立した自作oracle、必要時の所有PC版の挙動も併用する。時間・乱数・非同期完了が絡む全ゲームの再生一致は別作業であり、まず限定した入力／出力境界から比較する。

## Claude案の採否

| 提案 | 判断と必要な補正 |
| --- | --- |
| 関数単位の逆コンパイルと差分比較 | 採用。入力だけでなく更新メモリ、戻り値、必要なflags／FP状態、callback／TLS／例外／所有権も契約に含める |
| 当時のMSVCで機械語をmatching | 有力な証拠。ただし元compiler・オプション・ABIの特定が必要。matchingは元ISAでの対応を示し、復元C++をClang/ARM64へ移した後の一致まで自動保証しない |
| SKSEとRTTIからクラスを復元 | 補助資料として採用。classic版を対象の実行ファイルと照合する。RTTI名や公開headerは全field・全所有権・全thread規約の証明ではない |
| 全復元後に64bit化 | 一括移行の必須手順にはしない。guest側の32bit handle／layoutを維持して、host内部から64bit native処理を共存させる |
| atomicへ書き換えればTSO問題が消える | そのままでは不採用。共有状態の全アクセスとrelease/acquire等の同期関係を復元し、guest/native混在時も協調させる必要がある |
| Metal専用rendererを全面新規作成 | 長期候補。まず完全な描画入力・状態・resource寿命の境界を確定し、同じ処理単位を比較可能にする。Metal直結でも初回PSO生成は残る |
| 独自heapを捨てる | 一括では行わない。alignment、32bitアドレス、再使用、失敗、callback、副作用を調べる。既存bitmap案は測定で悪化したため再採用しない |
| ASTCへ事前変換 | 任意の別画質施策。ASTCは非可逆圧縮であり、既存BC→ASTC変換を「画質不変の最適化」と数えない |
| fixed timestepで120Hz物理を解決 | 別の互換性課題。物理stepだけでなく時間の所有者、AI/script、animation、入力、補間、collision/event順との結合を調べる。今回Havokの時間刻みは変更しない |
| Havok／Scaleformも全面復元 | 必要性と規模は未確定。既存互換経路を残し、局所原因が確認された場合に範囲を限定する |
| データMODは維持／native MODは不可 | 現在の保証にはしない。データMODも形式・拡張機能の個別確認が必要。x86 DLLをARM64として直接実行はできず、将来の再移植／bridgeは別計画 |

matching比較の[reccmp](https://github.com/isledecomp/reccmp)は関数・vtable・変数offsetの照合を支援する。ただし公式READMEの対応は旧MSVCの32bit x86が中心で、新しいMSVCは開発中。本実行ファイルへ無調整で使えると扱わない。

[MicrosoftのARM移行資料](https://learn.microsoft.com/en-us/cpp/build/common-visual-cpp-arm-migration-issues?view=msvc-170)は、未定義／処理系依存動作、alignment、浮動小数変換、メモリモデル差を挙げている。[Armのatomic解説](https://learn.arm.com/learning-paths/servers-and-cloud-computing/arm-cpp-memory-model/2/)も、共有データの順序を適切な同期で定義する必要を説明する。原x86でmatchingしても、そのソースを別compiler・ABIで最適化した結果は独立に検査する。

[SKSE公式](https://skse.silverlock.org/)はclassic 1.7.3／Skyrim 1.9.32とSE/AEを区別する。別版のクラス定義をそのまま移植しない。[ArmのASTC仕様解説](https://github.com/ARM-software/astc-encoder/blob/main/Docs/FormatOverview.md)は非可逆圧縮と明記しており、端末対応と元画像との差を確認してユーザーが選ぶ施策とする。配布可否はこの技術設計で判定しておらず、「ソースだけなら自動的に許可される」という前提も置かない。

## 実装単位と移行順序

1. **hostの重複作業を先に減らす。** 既に計測済みのstate-cache index、allocation/copy、同期不要な通知等を優先。shader初回生成はMSL／function／PSO／queue待ちを区別し、完全なkeyと寿命を持つ既存cacheを活用する。
2. **変換器の改善を独立して進める。** 既存のGPRローカル化・直接call・間接call cacheを再実装せず、未実施のper-bit flags生存解析、x87 TOP相対位置の静的化、証明できる呼出先の特殊化を比較する。ABIの慣習だけで状態を捨てず、解析不能な辺は元経路へ戻す。ARM出力が変わらない候補は採らない。
3. **狭いguest境界を読める形にする。** 今回はtextureのmip再コピーと別thread完了待ち。関数名の推測ではなく、元命令と呼出元・破棄側から契約を記録する。まだ全体のsourceを復元したとは言わない。
4. **guest ABIを維持したnative leafへ。** guest addressは固定幅integer、host pointerは別typeにする。範囲・alignment・alias・byte order・FP丸め・flags・副作用を明示。呼出前後に不要なCPU状態を丸ごと変換せず、必要状態だけを取り扱う。ただしlive-out flagsや例外経路が不明なら元経路を残す。
5. **安定したcomponentへ広げる。** resource preparationや所有権が明示された処理単位を、host job・cache・native objectへ移す。jobはguest TLSやcallbackを勝手に持ち出さず、公開順序を保つ。core数やクロックを固定せず、実際の依存と負荷に合わせる。
6. **必要な領域だけlayoutを独立させる。** 32bit guest objectとhost objectの境界をhandleで管理し、セーブ／ESP／ESMのwire formatをhostのsizeofやpointerから切り離す。全体64bit化や新rendererへの切替はcomponent単位で比較できてから行う。

大きな置換投資の入口には、同じプレイ区間でのコスト帰属表を置く。現ログから排他的な割合は計算できないため、未取得を明記する。局所の処理量削減や静的解析は並行して進めてよいが、全体の優先順位は実機のcritical pathで見直す。

各置換は「対象入力の固定 → 元命令の契約 → 独立fixture → ARM64の同境界比較 → 実機の同場面比較」の順に進める。matchingできる領域には元ISAのmatchingを追加するが、目的と無関係な全関数matchingを先に要求しない。平均FPSだけでなく50/100ms超frame gap、CPU起床、最大待ち、温度を確認する。ロードやmenuを含む集計はプレイ中FPSとして扱わない。

## 今回の3件から得た具体的な境界

- **texture view:** 自作2,031条件で同じ保持mip／header／payload。4K BC1、先頭1mipを除く例では保持payload 2,796,216 bytesの再コピーを0にできる。これは入力表現の試作であり、実loader・guestのallocator／failure契約・Metal pixel比較へ未接続。
- **macro variant共有:** 公開の実lookup/copy/destructorと自作Metal境界で8同時要求の生成8→1。例外、失敗library cache、再入fallback、warm/別keyの進行を確認。実Metalのdeadline／推移的lock契約をまだ満たしていないので本番patchへ入れない。
- **完了待ち:** constructor初期化、read/write双方のpending、複数の完了経路、callbackの結果格納、wait時の消費、destructorを追加確認した。破棄側に待機が見えない経路があり、上位のquiescence契約・全alias writer・動的取消／再使用は未証明。通知だけへ切り替えない。

実験コードは`experiments/`、元ゲームの限定解析はignored `private/analysis/stutter-goal-20261114-01/`。詳細な採否と次の検証は[性能記録](PERFORMANCE_WORK.md)へ記載。新GOまではlocal fixtureと解析に限定する。

## Claude追加レビューの検証（2026-11-14）

検索・実測前の提案を、固定公開source、既存の自作fixture、既に回収した端末ログと照合した。今回の変更は設計・記録だけであり、runtime、変換器、guest、cache設定、待機動作を変更していない。

### 変換器：既にある機能と新しく検討する機能

基準はSkydream `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a`と当projectの`patches/skydream/`。素のvendorだけでは、適用済みのcall-flags修正を見落とす。

| 候補 | 現sourceで確認した事実 | 次の限定作業・採用条件 |
| --- | --- | --- |
| guest GPRのローカル変数化 | `tools/lift.py::emit`でeax〜ediをCのlocalに展開済み。call境界はSPILL／RELOAD | 既存機能の追加実績には数えない。必要なら実ARMの余分な退避を特定し、calleeのread／write集合が証明できる辺だけ削減 |
| 既知callの直接化 | `call_direct`は既知entryを直接呼出し済み | 新候補は、定数伝播で証明できる間接先。可変先はaddress guard＋既存fallback。vtableを常に固定と仮定しない |
| 間接call高速化 | `lift_terminator`に呼出site別の最終target cacheがある。immutableなaddress／function pairを照合してから使用 | cache自体の再実装ではなく、実missと退避／reloadの残費用を分ける。推測したtargetの無条件直呼びはしない |
| flags生存解析 | 適用済み`0005-call-flags.patch`がentry／SPILL／RELOADでflagsを保持。明示的なper-bit interprocedural生存解析は現liftにない | instruction別のuse／def／undefined、CFG合流・loop、direct／indirect callとcallbackを扱う固定点解析。unknown edgeは必要状態を維持。C compilerが既に消した計算を改善と数えない |
| x87 stack静的化 | `CPU.st[8]`と動的TOPを使用。通常演算はdouble | まず同一basic blockからentry TOPに対する相対indexを解析。合流で矛盾、環境restore、未知call、再入はmaterialize／fallback。TOPだけでなくtag／status／exceptionと値の精度を保つ |

変換器変更は多数の関数に影響し得るが、全関数に必ず利益があるとは限らない。測定は生成Cの行数ではなく、同じcompiler／flagsでのARM命令・spill／load／store・コードサイズと局所実時間を用いる。関数数の拡大は、少数の自作入力で意味と機械語差を確認してから行う。旧SIMD memcpy案の「ARM出力同一」やheap案の悪化は再利用し、根拠なく再採用しない。

### ABIを適用する境界を証明する

通常のC/C++呼出規約でcallerがflags保存を期待できないことと、任意のx86 CALL／RETでflagsを捨ててよいことは異なる。解析で見つかったentryにはshared tail、compiler helper、assembly、callbackが混ざり得る。既存`tests/test_call_flags.py`は自作命令を使い、入力CF、戻りZF／AF、direct／indirect tail、nested call、host entryの8種類を元x86と比較する回帰試験である。これは特定のゲーム関数の使用頻度の証明ではないが、一律削除が不正である反例になる。

したがって「すべて通常ABIとし、検出した例外だけ除く」ではなく、証明した入口と辺にsummaryを付け、不明な辺は保存する。全calleeが既知でない場合に「例外が検索で見つからなかった」を証明にしない。DF=0は[MicrosoftのCRT契約](https://learn.microsoft.com/en-us/cpp/c-runtime-library/direction-flag?view=msvc-170)で裏付けられるが、未知のguest entry全体へリセットを入れる理由にはしない。

数学helperは通常のC引数規約とは限らない。[Microsoftの`_CIsin`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/cisin?view=msvc-170)はx87 stack上の引数と専用呼出規約を明記する。「call時は必ず空」として扱えない。静的リンクCRTのmemory／string処理は最初のleaf候補にするが、実binary範囲、呼出規約、DF、overlap、fault／過読、戻り値と副作用を照合する。`memcpy`と`memmove`を混同しない。`_ftol2`／`_CIsin`等は別群として、NaN／Infinity／範囲外、control word、戻りregister／ST0、status、精度と特殊入口を確認してから扱う。

### FPU精度：実ログにも対応不足の手掛かりがある

[D3DCREATEの仕様](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcreate)は、FPU_PRESERVEを指定しない場合のsingle precision／nearestを説明する。現bridgeの`B_IDirect3D9_CreateDevice`はguest flagsへMULTITHREADEDを足してDXVKへ渡すが、guest `CPU.fpu_cw`は変更しない。固定DXVKはFPU_PRESERVEなしで`SetupFPU()`を呼び、Clang/ARMの分岐では未対応の警告を出す。既存build5の回収済みprefixにも同じ警告が1件ある。これはこの分岐が実行された根拠であり、水表示や瞬間停止の原因という証明ではない。raw guest flags値と呼出前後のguest CWの実測は未取得。

現runtimeはguest threadごとにCPUを作り、初期CWは`0x027f`、MXCSRは`0x1f80`。fldcw／fnstcw／環境保存は状態を保持し、一部の整数変換／roundはCWのRCを読む。一方、`st`はdoubleで、通常の加減乗除はCのdouble演算へ展開され、PC bitsや全x87 exception／80bit挙動を完全再現していない。fieldを持つことを精度再現の完了と扱わない。

次の契約は、CreateDevice前後の同じguest threadのCW、後続のcontrol-word変更、PC／RC、SSE MXCSR、status、NaN／denormal／overflow、丸め境界を含む。元命令／所有PC側と現再コンパイル版の差、新native leafの差を別に記録する。CWの値だけ直して「修正済み」にしない。全演算をfloatへcastする案も、x87の指数範囲・中間丸めとは同じでない。丸めの互換性修正と高速化は独立採否とし、現再コンパイル版だけを絶対oracleにしない。

### TSO、切替、例外の追加契約

- **TSO分類:** plain MOVでのpublish／poll、lock解放、初期化を候補抽出する。pattern検出は同期関係の証明ではない。stackにもescaped pointer、SEH、APC／callback、別thread参照があり得るため、「stack address」だけでfence除去しない。object単位で全アクセスと寿命を確認する。LOCK命令の明示同期も前後の普通のload／storeと組み合わせて保つ。既存atomicと全fenceを今回変更しない。
- **切替:** leaf別のfeature maskを診断へ残す設計を採用。純粋なleafは呼出境界で選択できるが、allocator／cache／resource／async状態を変える置換はstartupまたは利用終了確認後に限る。同一セッションで任意に切替可能とはしない。shadow実行は入力を一度捕捉し、独立した出力／状態コピーで比較する。IO・時刻・乱数・callbackを二度発生させず、FP環境も復元する。
- **SEH／C++例外:** guestのTEB／fs:[0]とnonlocal jumpの契約を残す。初回leafはguest callbackを伴わない範囲を優先。native frameを跨ぐ未対応unwindを許さず、ABI adapterはnon-throwingにする。例外を捕まえて成功値を返すことで隠さない。callbackを許す段階ではguest frame chain、再入、失敗と復帰先まで比較する。
- **対象版:** 1.9.32という表示だけでなく既存の入力hashで固定。言語版等の違いもあり、別binaryのSKSE offsetやcompiler設定を流用しない。特定年代やversionの方が必ず高速という未実測の説明は採用しない。

### 3試作への追加指摘の採否

**Texture view:** 下流copyの棚卸しは採用。入力buffer→保持mip再構成→host Lock用buffer→DXVK managed backing／upload→GPUを、copy byte数・同時live bytes・待ち時間に分ける。Appleの共有メモリでもAPI上の重複bufferが自動的に同一になるわけではなく、logical sizeの単純和を物理footprintにしない。

固定DXVKの`d3d9_options.cpp`に`d3d9.evictManagedOnUnlock`は存在し既定false。`LockImage`／`UnlockImage`では、trueならmanaged stagingを捨てられる一方、後のLockでGPU→buffer copyとresource待ちに入る経路がある。再Lockで必ず不正になるという意味ではないが、メモリ削減と引換えにframe gapが増え得る。現在は有効化しない。実Lock履歴、再読込回数、同時live bytes、GPU待ちを確認して個別に採否を決める。今回のview試作は既存の読込済みbufferを借りるもので、新たなfile mmapを導入しない。将来mmapする場合はmapping寿命・file変更・初回faultのthread／区間を別に計測し、prefaultだけで以後faultしないとは扱わない。

**Macro共有:** 優先度逆転は有力な確認項目だが、現在のdeadline問題の確定原因ではない。[Appleのjob scheduling資料](https://developer.apple.com/videos/play/tech-talks/110147/)はownerの分かるmutex／unfair lockと、通知元が特定できないcondition／semaphoreを区別する。mapを短時間保護するlockを変えるだけでは、別GCD queueのcompiler完了待ちまで解決しない。長いcompile中にglobal map lockを保持してwarm／別keyを塞ぐ案は採らない。

timeout後の独立compileは候補だが、最悪が従来と同じとは言えない。仮に要求当たりの生成が1回以内でも、先に待った時間＋自分のcompile時間となり、late leaderとfallbackが並走してCPU／メモリ／熱を増やし得る。deadline共有、owner／generation寿命、late完了の公開規約、nested／cross-queue依存、最大同時生成を先に設計する。待機とMetal処理を強制cancelできるとは仮定しない。現試作の8→1だけで本番採用しない。

**完了待ち:** 「通知はhint、元pending条件を真実とする」案は条件付き採用。通知を失った場合の有限timeout再確認は有用だが、解放済みrequestを読み直してよい理由にはならない。元callerの寿命保証が使えるか、host wait recordのleaseと再使用世代、登録→条件再確認→待機、callback完了→状態公開→通知の順を確認する。timeoutは要求の完了・取消・解放許可ではない。同threadのalertable待機はAPCを処理する必要があり、そのまま非alertable event待機へ置換しない。通知喪失時は新timeoutまで遅れるため、既存1ms pollingより遅くなる可能性も比較する。現在は本番置換保留。

### コスト帰属と次の順番

既存の約1,043秒prefixはロード／menuを含む。graphics pipeline生成1,867回、平均約24.9ms、最大約571.6ms、100ms超220回。frame gapの100ms超は122回。これらは同時刻の1対1対応や原因割合を示さない。現在のhistogramはnested call・別threadの重なりを除けないため、guest／DXVK／MoltenVK／waitを足して100%にしたり、残差をguest CPU時間にしたりしない。

| 求める区分 | 現在の根拠 | 追加で必要な測定 |
| --- | --- | --- |
| guest CPU | 静的解析、局所fixture。プレイ中の排他的割合は不明 | 同じscene／時刻区間のthread CPU stack sampling、bridge境界と未知sampleの明示 |
| DXVK／MoltenVK | pipeline／Presentのhost経過時間。下位Metal段階の診断は次回候補 | thread／span／親子関係でinclusiveとexclusiveを区別。queue待ちと実行を別列にする |
| 待機／IO／fault | IO・frame gap等の分布。原因となる待機との対応は未確定 | blocking reason、起床、APC、page faultを同時刻へ対応付け。別thread作業時間と二重加算しない |
| GPU | このログにはGPU実行時間なし | GPU timestamp／queue timeline、present時刻、CPU側との同期点。CPU利用率から推測しない |

次の優先順は、(1)既に観測したshader／PSO停止を分解する計測と採用済みの重複削減を維持、(2)変換器のper-bit flags解析を自作CFGで設計し実ARM差の有無を比較、(3)FPU契約を確定してからCRT mathへ拡大、(4)textureの実loader境界と完了待ちの所有者を限定して接続条件を満たす、(5)実機同条件の割合を得て大型component置換を選ぶ。局所試作は並行可能だが、別機種／場面の数値を合成して改善率を出さない。Apple計測・新アプリbuild・更新は本人の新GO後にまとめる。
