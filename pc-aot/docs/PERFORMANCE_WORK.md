# PC版の性能改善作業 — 2026-11-13

## 2026-11-15 3系統の切替付き実装とPC検証

本人が「計測前にできる実装・試験をすべて」と指示した範囲を実装。app buildはGO待ちのまま。従来の「未接続試作／未実装」という節は、この節の状態へ進んだ。

- **待ち分離:** `patch_mvk_module_cold.py`が既存hot／library／timing patchを固定5fileへ合成し、dependency buildに接続した。`SKYRIM_MVK_MODULE_COLD=1`だけでmodule cold lockを選び、それ以外は旧global cold lock。決定はprocess内で不変。ready-hit・同module重複防止・公開／size無効化、共有compile失敗状態の保護、既存Metal monitor／timeout／callbackを維持する。診断converter guardはobject単位。旧experiment入口は互換wrapperとして保持。最終stage全体を別検査し、baseline fixtureも削らない。
- **保存:** native Settingsが表示中かつ本人のstartup opt-inがある時だけ、単一utility queue上でforeground admissionを試す。未保存あり・進行中compileなし・完了後10秒・前回試行から60秒を条件にする。main threadはdriver／registryを待たない。新compile、設定終了、非activeで取消。全saveをsingle-flightとし、寿命保護・3回のbounded snapshot再試行・64MiB上限・既存file保持を維持。完了時刻はatomic max、clock失敗は当該cacheのforeground保存を閉じる。Settingsはゲームをpauseしないので、driver snapshot／fsync中のframe停止が消えたとは扱わない。
- **保持と再利用:** opt-inでpipeline cacheだけをApplication Support/SkyrimIOS/ShaderCacheへ置き、backup除外に失敗すれば旧Cachesへ戻す。DXVKレシピ／logの場所と旧fileは保持。移行はtarget不在かつ旧identity／checksum／driver header一致時だけ。最初の公開はatomic no-replace linkで、途中にtargetが現れた場合やlink非対応では上書きせず明示failure。公開済みでpending掃除だけ失敗した場合はstored成功＋cleanup_errnoとして区別する。
- **互換性キー:** `pipeline_cache_namespace.py`とレビュー済みdigestのcontractによりstrict／reuseを作る。reuseが省略できるのは確認済みの計測header／present計測／Metal計測patchだけで、未知の版・不正contractはstrictへfallbackする。shader・serialization・その他critical source、identity algorithm、実SDK buildとcompiler version、deploymentの照合は維持し、入力変更はCMake再configureの依存にも登録。OS／deviceID／UUID照合は緩和せずopaque driver headerを書き換えない。初回導入やstrict↔reuse切替はcoldになる。同一binaryのmodule／prefetch切替だけではkeyを変えない。今後の任意の計測変更を自動的に互換扱いする仕組みではない。
- **先読み:** manager所有の`PrefetchGate`をdefault OFFで接続。required missはatomic scopeのみ、ready-hitは従来の早期return。先読みはvariant境界のlock外で譲り、compile mutex取得後も再確認する。1variantで同じ絶対期限1秒を使い、2ms刻みで確認し、高温／低電力は8msのbounded pacing。clock異常や累積3回の期限超過で従来動作へ退く。終了前に停止flagを立て、例外時はRAIIでrequired数を戻す。開始済みcompile／mutex待ちやOS timer遅延に1秒上限を保証するものではない。pending jobを捨てず、worker本数・QoS・画質／FPSを変えない。ログはlock解放後、進捗最大5秒1回・期限通知最大3件、errno保持。

**設定と統合:** `Shader trials — applies after restart`へparallel／priority／settings-save／persistent-storage／compatible-reuseの5項目を英語default＋日本語選択で追加した。全て既定OFFで、startup snapshotからのみ有効化する。1項目ずつ同条件で比較できる。設定を閉じる／非active時はforeground permissionを即時取消し、foreground／既存background保存は同じserial queueを使用。新しい依存buildの既定先を`local/ios-dependencies-shader-trials-v1`へ分離し、旧build7の成果物を上書きしない。次のMac検査へ新namespace／gate／composed-module検査を登録したが、実行はしていない。

**実行した検査と修正:** module実methodはnormal2検査18.447秒、ASan/UBSan2検査49.150秒、TSan2検査21.015秒。その後の最終stage drift／既存4adapter整合は5検査23.264秒で成功。cache ownerはtimestamp逆転・clock失敗・移行途中collision・link非対応・pending掃除失敗を追加し、最終normal7.860秒／ASan-UBSan14.156秒／TSan7.520秒で成功。gate＋実Graphics/Compute method等は関連8検査175.382秒、ASan/UBSan3検査、最終共有ログを含むTSan3検査70.092秒が成功。8要求を同時期限超過させてもログ3件に留まる追加TSan1検査5.854秒も成功。TSanは既存の明示指定による子processだけのASLR対処を使用し、app／system設定を変更しない。

親側ではnamespace安全性＋実CMake identity blockの5検査が成功（Windows CMakeでconfigure-only、Apple SDK buildではない）。旧overlay5検査は成功。stall fixtureは追加したrole histogramをクリアし忘れて出力が1行多くなる失敗を検出し、fixtureが全contextをdrainするよう直して2検査が成功。新しく生成したDXVKのgraphics／compute／shader／state_cache／pipemanager／pipecacheの6実translation unitをGCC13 -fsyntax-onlyで検査した。最初のpipecache検査は手動検査コマンドにCMakeが与えるnamespace defineが欠けて失敗し、それを供給した再検査が成功。source preflight・差分・3shell script構文も成功。試験の重複回数をiPhone frame数や効果倍率と数えない。

**残る採用条件:** Apple SDKでの本体・UIKit・最終ObjC++構成、実Metalの並行failure／readback、iPhoneで同save／画質／経路のcold・warm・高温比較は未実施。新build／push／署名／配布／端末更新なし。特にforeground snapshotのcatalog lock、MSL compilerの内部混雑、先読み遅延による後続miss、起動時間・headroom・heatを確認する。平均FPSだけでなく100ms超gap／draw準備時間／failureと描画結果で判定し、悪化する項目はOFFへ戻す。

## 2026-11-15 Gemini review統合：キャッシュ互換性の見落としと役割別pipeline計測

**独立確認した訂正:** 固定MoltenVKの`MVKDevice.mm`（SHA256は既存pinの1561014e…と一致）の`getHighestGPUCapability()`はOS major／minorとGPU familyをまとめ、それがiPhoneの`deviceID`と`pipelineCacheUUID`双方へ入る。従ってアプリ側の`uname.release/version`だけを外しても、driver側識別が変わるOS更新を跨げない。同一OS major／minor内の修正更新とは区別する。driver headerを書き換えて受理させる変更はしない。`readData()`はheader照合後、全entryをdeserialiseし`addShaderLibrary()`からMetal libraryを生成する。実際の新要求と`matches()`するより前の処理なので、後段のconfig一致だけで読込全体の「100%安全」を証明したとは扱わない。

**保存時期の制約:** `writeData()`はflags=0でcatalog lockを保持したままCerealの計数／書出しを行う。既存hot-cache変更が外へ出したのはcold生成であり、この保存lockではない。`compiled()`は成功したpipeline APIの完了を数えるため、増加が止まっていても進行中のコンパイル、通常描画、I/Oが静穏とは限らない。条件確認後の新規仕事もあり得る。checkpointの1秒予算や取消は実行中のdriver／fsyncを中断する上限ではなく、registry lockはcheckpoint中のcache破棄を待たせる。低QoS化だけで描画非阻害とはしない。switcher終了が毎回未保存になる、offered_bytesが実hit数を示す、同ルートならroot missが必ず0になる、という断定も採らない。

**共通方針と分割:** FableとGeminiが支持したmodule単位cold分離候補は維持するが、実MetalとiPhone比較前に採用しない。発熱中のworker本数は自動で減らず、起動時の本数とitem境界のQoS変更は別である。先読み抑制、FIFO化、QoS overrideは効果と飢餓／終了処理を別に検証する。キャッシュ改善は（a）build7の保存outcome／取消／復元とroot・macro実測、（b）シェーダー出力以外にserialization／ABI・入力検証・runtime機能も含む名前空間の依存確認、（c）安全な保存機会とdriver snapshotの待ち、（d）容量上限・backup除外・旧path保持を含む保存先移行、へ分ける。OS照合緩和は別の後段課題。BinaryArchiveは後段PSO再生成の候補であり、本経路のMSL library生成を省く変更ではない。圧縮方式やMetal 4への全面移行も優先しない。ゲーム由来入力の外部送信禁止と、本人所有データのPC内解析は混同しない。

**今回の実装:** `performance_metrics.c`にgraphics／compute pipeline各3role、計6枠の固定histogramを追加。既存API区間とTLS roleを利用し、新規のclock／lock／allocationはない（集計用relaxed atomicsは増える）。既存kind番号・header・DXVK／MoltenVKパッチ・compile順序は不変。`analyze-frame-timing.py`は新しいrole別行と失敗数に対応し、source／wait-result付きの誤ったpipeline行を拒否する。旧行も読め、全体集計にrole別を二重加算しない。この時間はVulkan pipeline API区間で、呼出し前のDXVK compile mutex待ちは含まず、CPU実行時間や待たせた所有者の証明でもない。

**検証:** native recorderの4 producer・80,000件について全体とdraw／prefetch各40,000件の集計、各失敗10,000件と時間総和を確認。実C logger→Python解析の往復でgraphics／compute全role、非0の継承source/resultの正規化、失敗分類、100ms境界、旧形式と不正入力を検査し、関連2suite・12検査が62.095秒で成功。ASan／UBSan付きの並行記録1検査も1.962秒で成功。source preflight・差分検査も成功。CMake本体と名前空間に含む17fileがHEADとbyte一致するため、この計測差分だけでは既存cacheを無効化しない。SDK／compiler／他入力変更がない場合の限定で、一般的な更新互換性保証ではない。

次の実機比較ではdrawのpipeline／MSL時間とframe gap、失敗・描画・headroom・thermalを同じ経路／設定とcold・warm条件で見る。cold_lock_waitだけの減少や、合計待ちの重なりを因果と誤認しない。build7の新しいログは未回収で効果量は未確認。新しいcloud build／push／IPA／端末更新は実施していない。

参照：[固定MoltenVKのGPU識別](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm)、[固定cache読込・書出し](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm)、[Appleのdevice-built archive説明](https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects)。

## 2026-11-15 Fable review：試験の境界を訂正し、実converterの戻り値を限定再現

本人が中継したFable回答を、固定MoltenVK／DXVK、現build7のpolicy・計測、Apple pthread資料、Vulkan仕様と照合した。既存の24条件512比較とsanitizer結果を再利用し、今回変わったfixtureと新しいerror probeだけを通常実行した。reviewは設計支持の材料であり、Apple／iPhoneでの採用承認や性能証明ではない。Geminiの別観点reviewと合わせて最初の製品変更を絞る。

**確認・反映した点:**
- 冷えた状態で起動した6core例ではworker3本が作られ、その後のthermal／低電力ではworker item前のQoS判定が変わるがthread数は作り直されない。module分離で同時進行可能なroot cold生成は当該CS1＋worker3の範囲で増え得る。macro生成は元からcache cold lock外にもあるため、全Metal仕事が従来1本だった、service内部が必ず4並列になる、とは扱わない。
- flags=0のmergeはdst lockを内部取得しない。[Vulkan仕様](https://docs.vulkan.org/refpages/latest/refpages/source/vkMergePipelineCaches.html)上も、INTERNALLY_SYNCHRONIZED_MERGEなしでは呼出し側がdstCacheを同期する。DXVKのこの経路はmergeを使わない。自作`mergeReady`を`publishReadyForTest`へ改名し、catalog保護された競合公開／敗者cleanupだけの検査と明記。実merge API／flags=0並行mergeの検証とは呼ばない。
- `_conversionLock`はmodule objectごとで、同じcontent keyを持つ別object間で共有されない。異なるcacheから同名dumpへ書く経路までは保護しない。候補comment／READMEを限定し、診断機能全体の並行安全性は未検証として残す。製品の通常変換へ新mutexを追加する変更はない。
- 実SPIRVToMSLConverterのwasConvertedはtrue初期化後、CompilerError catchでfalseへ変わらない。空の自作SPIR-Vと空configをconverterへ直接渡す1ケースで、stderr／resultLogの変換error・空MSL・true返却を再現した。これはinvalid-inputのCPU境界検査で、vkCreateShaderModuleの入力検証を通しておらず、valid game shaderの到達性や全catch後処理の安全性を証明しない。既存のfailConvert注入が「実converterの失敗を正しく再現」するという解釈は撤回する。製品の戻り値／失敗library方針は今回変えず、独立の不具合候補として記録。
- 旧build6の終了済み4ログだけをPC内で再検索し、SPIR-V→MSL conversion error、Metal compile failed、graphics_pipeline failures>0の窓は各0件。生logや名前を外部へ出さない。記録内で未観測という意味で、全実行に不存在／現在のカクつきの原因とする意味ではない。

**回答から引き継がない断定:**
- A=draw graphics-lock、B=draw cold-lock、C=prefetch cold-lock、D=draw MSLの集計だけで因果は確定しない。Bには必要な同module合流が含まれ得るため「B大ならmodule分離が直接効く」とは言えない。AとCが同時に大きくても同じ依存鎖とは限らず、Dにはcompiler内部のqueue待ちも含まれる。個別span、同module／別module、owner世代・公開時点の整合を補い、未知は未知に残す。
- first-error VkResultが後で読まれなくてもplain値への並行writeはC++ data raceであり、共有error公開の保護は形式だけの対処として降格しない。catalog→Metal／module待ちを新設しない条件と、遅延callbackのowner寿命を維持する。
- [Appleのfairshare](https://raw.githubusercontent.com/apple-oss-distributions/libpthread/main/man/pthread_mutexattr.3)はFIFOであり必須描画を優先する仕組みではない。待機者flagの取得前後snapshotだけでは、OS待機列への登録順を証明しない。後続workerの取り直し軽減候補として、正しいiOS mutex生成・対象pipeline限定・shutdown・実機の待ちを別途確認する。QoS overrideも所有者thread寿命、解除、非同期Metal serviceへの効果、熱の代償が未検証。どちらも今は導入しない。
- metalCompileTimeout既定INT64_MAXは実用上ほぼ無期限だが、全設定・全経路で遅延callbackが存在しない保証として既存timeout／destroy契約を削らない。
- MSL分布の悪化だけでcompiler混雑と断定せず、温度・scene・準備対象数・cache温度・録画等を揃えて比較する。warm再訪・再起動も含める。cold_lock_wait自体の短縮は、lock範囲を変えた結果でもあるので採用の単独指標にしない。

**次の最小観測候補:** build7はMSL role/sourceとcompile/cold待ちを分類済みだが、graphics_pipeline全区間のrole別histogramは未実装（長いstall spanのroleと区別）。変換区間とrole別graphics全区間、必要なら同module／別module競合を固定量で追加する案を、Gemini回答と統合して決める。まず現build7ログの再利用で足りる情報を読み、新しい観測を必要な境界へ絞る。

**今回の実行:** 追加error characterizationを含む実converter通常1検査47.064秒成功（既存512比較も一致）。名称とcommentを限定したmodule method通常2検査19.260秒成功。並行algorithmは変更しておらず、そのsanitizer検査を繰り返さない。実Metal failure注入／readback、Apple compile、長時間・温度域別のiPhone比較は未実施。review packet `local/consultations/Skyrim-shader-review-20261115.txt`は送付時点の資料として上書きしない。新Mac build、private再compile、push、署名、端末更新は行っていない。

## 2026-11-15 性能優先へ固定：実converterで追加排他の必要範囲を確認

本人はカクつき・安定性等の性能を最優先とし、MODはその後、multiplayerは当面保留と指定。MOD_MANAGEMENT_DESIGN.mdにも反映。追加のcloud build／push／署名／端末更新をせず、build7・private guest・原ゲーム／save／旧IPAを保持する。

**前の保守的なconverter排他を修正:** 固定SPIRVToMSLConverterはSPIR-V vectorを保持し、通常convertではCompilerMSLと付随状態を呼出しごとに作る。callerのconfig／resultも別物である。`tests/test_mvk_converter_parallel.py`で固定MoltenVK converter・SPIRVSupportと実SPIRV-Cross 6unitをcompileし、既存のconfig比較／sampler alias修正を含む実変換を検査した。自作の色／深度alias順違い・別bindingの3program×MSL版／binding／MSL・GLSL結果logの8条件＝24条件を使用。直列referenceと、8thread×32回の外側排他あり／なしの各256回を比較し、MSL・result log・conversion metadata・used-resource flags一致、入力SPIR-V不変を確認した。512回は比較回数であり、実機frame数や性能倍率ではない。

通常GCC13は1検査45.024秒、TSanは1検査48.167秒で成功。最初のportable compileはApple側precompiled headerに相当するcstdint不足で失敗し、test compileへ明示includeを足した。最初のTSanは`unexpected memory mapping`で起動できず、pass扱いせず、`SKYRIM_TSAN_DISABLE_ASLR=1`による子process限定setarch -Rで再実行した。OS全体やappのASLR設定は変えない。実SPIRV-Tools disassembly／file dumpは対象外（upstreamの除外optionを使用し、export到達時はabort）。実Metal・Apple SDKの成功を模したものではない。

この根拠から`experiments/mvk-module-cold/patch.py`の追加conversion lockはdebugModeまたはshaderDumpDir有効時だけ取得する。通常経路の複数cache／cacheなし変換を新たなmutexで直列化しない。同module cold生成の順序、ready-hit、catalog公開、共有plain VkResultのfirst-error保護、Metal device monitor／completion／timeoutは保持。旧節の全面converter排他案はこの限定案で置き換える。

**修正候補の検証:** 実method fixtureでnormal cross-cache進行、diagnostic排他、Metal待ち相当中のconverter解放を追加確認。重複生成0、4,096 vector成長lookup、16,384並行error公開と既存失敗／寿命検査も維持。GCC13 ASan/UBSanは2検査44.285秒、TSanは2検査18.721秒で成功。後者も検査子processだけ上記ASLR対処を使用した。新たな全private compileや無関係なsuiteの繰返しは行わない。

候補は引き続き未接続。実Metal並行compile・failure・readbackと、同save／画質／経路のiPhone比較は未実施。同時compileの増加がMetal compiler混雑・peak memory・熱へ与える影響も未確認であり、通常変換の出力一致や局所待ち分離をFPS改善とは呼ばない。build7ログのroot／復元／macro、必須／先読み、cold lock待ちを確認して採用対象を絞り、次のMac buildは本人GO後にまとめる。

## 2026-11-15 続行時のsource再点検と将来機能の切り分け

LE MOD／multiplayer調査はMOD_MANAGEMENT_DESIGN.mdへ統合した。今は第三者MODを入れず本体性能を優先する。cold分離候補では実SPIRVToMSLConverterも読み、保持memberがSPIR-V vectorで、通常convertはCompilerMSLを毎回newし結果とconfigを呼出し側へ返すことを確認した。従来の「converterが非constだから共有状態を変える」という推定だけでは追加mutexの必須性を証明できない。前回の自作境界検査はmutexの排他動作を示すもので、実変換器のrace再現ではない。既存candidateは未接続のまま保持し、通常経路とlog／dump等の補助経路を分けて最小の必要同期を見直す。compiler失敗時のplain VkResult共有は別の具体的な問題であり、専用の短い公開lockによる保護候補は維持する。今回新たなbuild／shader変換ベンチ／実機比較は行っていない。

## 2026-11-15 追加候補：moduleごとのcold生成と共有失敗状態の保護

ユーザーの「今あなたの方でできる他のことはもうない？」を受け、build7ログ待ち中に独立して進められる最小の待ち分離候補を実装した。`experiments/mvk-module-cold/patch.py`は既存build7 hot-cache／library／timing adapterを合成した固定MoltenVK 45887054だけを受け付ける。製品のdependency buildには接続しない。元入力へ直接書かず、全入力を先に照合し、stage再適用も同一結果にする。

- **待ち分離:** `_shaderCompileLock`をcache全体から安定した各`MVKShaderLibraryCache`のcold用mutexへ移す。ready検索・再検索・vector公開・markDirtyは既存catalog lock下で行う。同module別variantのcold生成は直列のまま、同moduleのready-hitはcold待ちを通らない。未変換configのmatchesを同一key判定に使わず、worker増設／QoS変更／画像やCPU ABI変更もない。
- **converter:** 実`MVKShaderModule::convert()`全体だけを別mutexで保護。複数VkPipelineCache／cacheなしから同じmoduleを使う場合も内部converterを同時に変更しない。Metal library構築と完了待ちへmutexを持ち込まない。
- **追加で確認した失敗経路:** 固定`MVKConfigurableMixin`はplain VkResultでfirst-errorを保持する。`MVKMetalCompiler::handleError()`はownerがcacheでもこの値へ直接触るため、cold並列化だけでは共有失敗状態が競合し得る。候補はcache ownerのerror公開だけを既存catalog lockへ通し、他ownerは元のsetterへ戻す。library生成中にcatalog lockを保持しない。cacheのreadData失敗／構成結果読取りは作成時、runtime compiler失敗はAPI使用中という寿命を維持する。遅延callbackへowner参照を追加せず、completion／timeout／destroy本文は不変。
- **維持・限界:** 全cache serialization／merge本文、外部同期経路、nil-library cache、feedback、Metal device monitorを保持。catalog snapshot検査は個数・size metadataを扱うだけでCereal／実driver payloadの検査ではない。同module coldやmacro特殊化の待ち、初回MSL compile自体を除去する変更ではない。

**検証:** `tests/test_mvk_module_cold.py`は固定ソースから実cache method・実convert本文・実compiler error本文・非atomic first-error mixinを抽出し、実MVKSmallVectorを使う。SPIRV-Cross／Metalの実行部分は自作境界であり、実shader変換・GPU描画の成功とは扱わない。変換中／library生成中の停止を注入した2ケースとも、旧方式では別moduleのcold要求が待機し、候補では停止解除前に完了。2module fixtureでは進行可能なcold処理が1→2、同module duplicate8要求の余分な生成は0。same-module ready-hitとcatalog snapshot通過、shared converter同時進入最大1、変換失敗／例外、構築例外、公開bad_alloc、merge競合、外部同期、nil-library保持、実vector成長中4,096lookup、32回×8thread×64回＝16,384error公開とfirst-error保持を確認。

GCC13通常2検査は13.997秒、ASan/UBSan 2検査は39.420秒、TSan 2検査は16.489秒で成功。今回TSanは起動・実行まで成功した。初回はfixture内のvendor include分類とtestのindent警告でcompileが止まり、修正後に上記を実行した。runtime候補の動作失敗を隠してskipしたものではない。生のゲームログ・ゲーム由来codeは使わず、クラウド送信／課金／Macビルド／署名／端末更新はしていない。

**採用前:** Apple SDKで全体をcompileし、実Metalの並行compile／失敗／readbackを確認した上で、同条件のiPhone比較が必要。同時MSL生成の増加はcompiler service混雑、peak memory、発熱を増やす可能性がある。build7のroot／restore／macroと必須／先読み分類を読み、実際の停止にこの待ちがどれだけ含まれるかで採用を判断する。局所的な待ち分離の証明をFPS改善とは呼ばない。現build7はそのまま保持する。

## 2026-11-15 build7の初回短時間プレイ報告

本人は短時間のプレイで「60fps近くで前より安定」「カクつきも大きく減ったかもしれない」と報告。狼の出現後、戦闘へ入ろうとした時に一瞬停止した。体感の改善傾向として記録し、未回収の新ログや同条件ABで確認した数値とは扱わない。shader／描画状態の初回準備、素材読込等は候補であり、狼との遭遇だけから原因を決めない。更新による改善・cache蓄積・設定／場面差の寄与は未確定。今は追加プレイを求めず、後で今回のログを分類して直接対策の対象を選ぶ。

## 2026-11-15 build7のMac検証とOTA準備完了

本人GOを受け、public hostだけを公開`fdaaae1c9233dd2bc5aafeeb6c60ceec118b45e2`としてCodemagic build21で1回実行し成功。新dependency stage build7を使い、旧cacheを保持した。34組83検査の81件成功、Windows専用ARM検査／任意FFmpeg decoder検査の2件skip。実MetalはiPhoneOS8shader compileとMac GPU20自作入力ケースの数値照合が成功。新しいperformance context／cache checkpointのfixtureと実Apple compile/linkを通過したが、iPhone stutter改善は未計測。

公開host SHA256は`65a38a91766ab032d7527d952ee0ce362100371c67bb9ec17da1d21ad9c9e51c`。成果物／原logは`local/codemagic-title21/`。x86.h／rt_table.h／PcGuestABI.hはbuild6とbyte一致。CPU headerは既存private guestのheaders.x86.h、PcGuestABIはabi_header_sha256と一致。private frameworkは未変更の`dedd20f0e42d4c24b10f7f10b193b5987c4bd45a069ac34e8cc263efe7c86d48`、binding manifestは`1da3244849c8d83538c2fb06a6e15e82e81f5b112424adfa6f10708baaee46be`。PC内で組立・既存署名し、2images／39,426pages／4resourcesとprofileを検証した。旧IPA／原ゲーム／saveは保持し、private guestの再compileなし。

署名済みbuild7は58,672,647bytes、SHA256 `1c7dd588ab5adba482a59db2836764bbc51310b1a001e311281f9bbb111e15b5`。private OTAの既存routeだけ更新し、manifestのbuild／AppID、HTTPS全転送後hash一致、390pxの英日表示を確認。本人へ保存・完全終了・Tailscale接続後の上書き導入を案内済み。端末導入／起動／save表示／改善は未確認。無料枠は198/500（残302分）。次は同save／画質／経路で初回と反復、背景保存を挟む再起動を比較し、root／復元／macroと必須／先読みの分類を読む。今回の計測だけでlock所有者や因果を確定しない。

## 2026-11-15 USB不要のOTA更新を実機で確認

ユーザーの希望により、既存開発者署名を保持したbuild6の非公開配布経路を準備。Tailscaleの既存アカウント／HTTPS設定を再利用し、PCのGUI起動後にNoStateからRunningへ復旧。既存proxyを保存し、SkyrimIOS専用のsubpathだけを追加した。Funnel／一般公開、署名鍵の移動、外部hostへのIPA upload、新規cloud build、再署名は行っていない。

配布helperはscripts/serve_private_ota.pyとprivate_ota.html.in。127.0.0.1:8765のみでlistenし、Tailscale Serveが設定する同一利用者headerと256bit相当のランダムURL、24時間の期限を確認する。配布対象は固定IPA、manifest、案内pageと数値statusだけ。directory listing／任意file読出しはない。config／URL／IPAはignored private/以下。元IPA 58,670,330 bytesのSHA256を既存署名結果と照合し、HTTPS経由の全転送後にも一致を確認。アプリidentity、既存profileの期限／登録端末／メモリ3権限を確認した。HEAD、単一range、誤identity／誤URL／範囲外／期限切れを限定確認し、英語・日本語のmobile表示を実ブラウザで点検した。公開source検査も成功。

**本人確認:** Tailscale接続後、最初のページ接続失敗は再確認で解消。Safariのpage表示に続き「インストールでき、起動・セーブも確認できた」と報告。現在と同じbuild6の無線上書きであり、性能改善のAB比較／端末内saveのbyte比較ではない。新しいcache診断・保存改善コードは未ビルドのまま。元ゲーム・既存IPA・署名資格情報を保持し、ゲームデータの再コピーやアプリ削除は行っていない。

運用: private/ota/config.jsonを参照し、検証済みの新IPAへ更新するときはhash／build／期限を更新してhelperを再起動する。現在のhelperはexec session80531、元session61680は当方のpage調整時に停止済み。現configは翌日まで有効。Tailscale Serveの既存root proxyを削除・resetしない。停止するならSkyrimIOSのpathだけを無効にし、当方のhelperだけを止める。ネットワーク公開・build・署名を自動開始する仕組みは追加していない。

## 2026-11-15 実装：background保存の限定retryと待ちの分類

ユーザーの「このあとはどうする？実装？」を受け、上記相談の最初の範囲を実装した。公開host側だけの変更で、ゲーム入力／private guest／CPU ABI／画質／worker数／QoS／同期順序／cache形式を維持。Mac build、push、署名、端末操作は行っていない。

**保存:** `ios/dependencies/pipeline_cache.cpp`はcheckpointごとに終了時1行を出す。取消・変更なし・pathなし・driver error・容量外・試行終了・I/O失敗・例外・invalid payloadを区別し、stage／attempts／VkResult／取得size／bytes／snapshot generation／saved generation／保存errno／elapsedを数値で残す。`checkpoint_all`は訪問cache数と取消を別に記録し、全cacheの保存成功という意味にはしない。UIKitはprepare／queue／finishを記録し、復帰取消・期限切れ・task取得失敗・重複投入なしを区別する。

- data queryの`VK_INCOMPLETE`または成功だが36 bytes未満の場合だけ、fresh size queryから最大3試行。別driver error、上限超過、invalid header、書込み失敗はretryしない。64MiB payload上限は維持し、vector容量が足りないときは旧allocationを先に解放してから確保する。
- 1秒は取得処理を次へ進める許可の予算。各call後・次のcall前・確保後・保存開始前に取消／予算を確認する。進行中driver call・fsyncを強制中断する機能でも、checkpoint全体を1秒以下にする保証でもない。sleep／foreground timer／WillTerminate待ちはない。
- 成功した試行のsize query前に読んだgenerationだけを保存済みにする。後続compileをclean扱いしない。既存registry lockによるcache/device寿命保護とpending→fsync→close→rename、旧公開file保持を維持する。
- checkpoint `outcome`: 0 stored、1 clean、2 no path、3 cancelled、4 driver error、5 size range、6 retry/予算終了、7 write failed、8 exception、9 invalid payload。`stage`: 0 start、1 size、2 data、3 pending除去、4 open、5 write、6 fsync、7 close、8 rename、9 done。`budget_expired`で予算終了を区別する。UIKit finish sourceは0完了／1復帰／2期限切れ。

**分類計測:** `performance_metrics.*`に固定のTLSスコープと33個の固定histogramを追加。記録処理は追加heap確保・file I/O・待機なし。既存5秒集計時にアクティブな分類のみ`IOS_TIMING_CONTEXT`へ出す。既存`IOS_TIMING`のkind番号は維持し、cold lock waitだけ末尾へ追加。`IOS_STALL` schema2にはrole/source/resultを追加し、解析器は旧schema1も読む。分類histogramは総量の内訳であり、総量へ足さない。

- role: 0 other/unknown、1 getPipelineHandle必須要求、2 compilePipeline先読み。既存ready hitはスコープを作る前に返るので、新しいclock／histogram／待機は加わらない。ネストや例外で元TLS状態へ戻す。
- source: 0 unknown、1 root MSL source constructor、2圧縮root復元constructor、3 macro派生constructor。これはlibrary取得命中率ではなくMSL compile待ちの発生入口を表す。
- DXVK compile mutex競合は元の待ち開始・取得時刻を保存し、list lockを解放した後にresultを付けて1回だけ記録。result 1は待ち後hit（cached null含む）、2は生成経路へ進む（生成成功の意味ではない）、0は未分類。無競合のready hitは記録しない。所有者交代や公開後追い越し時間はまだ計測しておらず、これだけで先読みが原因とは断定しない。
- MoltenVK cold lockは同じ排他と1 cache 1 compilerを維持し、競合した取得待ちのみ記録。Metal dispatchは親の数値contextを値として渡し、共有GCD threadのTLSを書き換えず、元block完了後のcompiler objectへ新しくアクセスしない。既存のtimeout／遅延callback／failed library方針を維持する。
- stall枠は4 frame＋12 workのまま。分類histogramはspan欠落と独立して件数を蓄積するが、有限span／dropped_busy／dropped_full、時間重複が因果を証明しない制限は残る。無根拠な完全帰属や改善率を出さない。

**検証:** 自作Vulkan boundaryで競合1回→2試行成功、常時競合→3試行終了、short成功、取消後callなし、2回目容量超過、別errorの即終了、clock予算終了、例外1行、旧file／pending／成功試行generation／device lifetimeを確認。実DXVK lookup/publication fixtureはGraphics・Compute、hot-hit通過、同key1生成、null／例外／再入／8thread混在を保持し、要求roleと元contextへの復帰を確認。実MoltenVK constructor／cache／compiler waitを使うfixtureで並行publish・ready-hit／snapshot通過・復元／macroタグ・timeout／遅延callbackを確認。C recorderは80,000件の並行分類を混線なしで集計し、旧・新ログをparserで検証した。

通常の該当suiteが成功し、追加のGCC13 ASan/UBSan 7検査も成功（43.802秒）。Clang18 sanitizer runtimeは未導入でlink不可だったため、依存追加はせず既存GCCで実行した。実DXVK graphics／compute／pipecacheの3 translation unitもClang18で構文検査成功。新しいshader lock algorithmは入れていない。TSanとApple SDK／UIKit／実Metalの実行検証は今回実施していない。開発途中の旧6列期待値・旧ログ行数期待値とPython bytecodeの古いpin参照は補正し、該当検査を再実行して成功した。

**次:** GO後の一括Mac検証・更新で、同save／同画質／同じ経路の初回と再訪、background保存完了を挟む再起動を比較する。依存namespaceは公開header／patchを含むため今回もcold開始の可能性を明示し、warmなbuild6との単純比較をしない。root／復元／macroと必須／先読みの内訳を得て、必要なら所有者交代を追加観測してから優先gate・module分離を選ぶ。2ms polling、常時QoS昇格、device monitor削除、foreground保存、BinaryArchive、ゲーム側の逆コンパイル置換はこの変更に混ぜていない。局所検査の成功は実機stutter／FPS／発熱改善の証拠ではない。

## 2026-11-15 Claude再回答：最初の実装範囲と残る補正

相談内容を統合し、最初の対象を既存background checkpointの固定量理由記録・限定再試行、および描画待ちの分類に絞る。製品への接続、cloud build、push、端末更新はまだ行っていない。前のcache競合／gate／matchesの検証結果を再利用し、今回は新しいcompileやtestを繰り返していない。

- **checkpoint案は説明と擬似コードを揃える。** 提示コードはdeadlineをraced判定後にしか調べず、size queryが予算を使い切って正常終了してもdata queryへ進み、正常data後も保存へ進む。cancelledも各driver呼出しの直前には検査していない。実装時は新しい試行・size query・確保後のdata queryを開始する前と、各callが戻った後で取消／予算を検査する。driver APIやfsyncの途中は強制中断できないので、1秒は新しい取得処理の開始許可の予算であり、全処理の実時間上限ではない。retryはdata側VK_INCOMPLETE／明示した短いpayloadに限定し最大3試行、他error・上限超過・書込み失敗は即終了。既存の旧file保持・pending cleanup・generation方針は維持する。
- **診断の負荷と意味を固定する。** reportは固定長・noexceptで1試行ごとの大量出力を避け、checkpointの終了理由を1行、UIKit遷移／取消は別eventへ分ける。catch側の重複出力、stale errno、stage未設定、取得失敗と正当な小cacheの区別を確認する。既存parserのbytes/stored等と互換を保つかschemaを明示する。64MiBはpayload上限であり、vectorの再確保中のpeak memoryまで64MiB以内という意味ではない。
- **背景処理も復帰との競合が残る。** Utility worker中のdriver取得／fsyncはresume直後に停止できず、registry／cache lockを介した待ちも残る。「backgroundだけなのでプレイへ影響しない」とは保証しない。WillTerminate待ちとforeground定期保存は初回変更から外す。
- **2ms polling gateは製品採用を保留。** 必須側のmutex除去とvariantごとの絶対deadlineは改善だが、3workerが継続して待つ条件では設定上約1,500確認/秒になる。実起床や発熱増加を測った値ではないものの、電力の目標に照らして検証が必要。[Appleのtimer指針](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/MinimizeTimerUse.html)も同期用pollingの起床負荷を説明する。sleep_forの再開はscheduler次第でありD+SLICE／shutdown SLICE以内という実時間保証はしない。
- **gateの新しい擬似コードにも重複計数がある。** 同variantで期限切れ後に、外側whileのmayStartとcompile mutex取得後のmayStartがそれぞれPREFETCH_YIELD_EXPIRED／expiredStreakを増やす。期限切れ記録1件という試験条件と一致しない。variant単位のexpired状態で一度だけ記録し、streakが複数worker間で何を数えるか明示する。atomicの順序は開始許可を定義できてもstd::mutexの公平性を保証せず、往復回数をworker数で抑えられるとはしない。
- **macroが主体でもDXVK gateの対象外とは限らない。** macro生成はMoltenVKのcache cold lock外だが、上流DXVKのcompile mutexを保持したvkCreate*Pipelinesの内側で発生し得る。先読みmacroが必須要求を塞ぐなら先読み開始制御は候補。初回compile自体が消えるという意味ではなく、root／復元／macroだけでなく必須／先読みの組合せで判断する。device monitorは内側のspecialize／dispatch最大値だけから全取得箇所を除外できない。最初の追加計測で後回しにすることと、原因から排除することは分ける。

初回計測は、MSL生成由来、必須／先読みの要求起点、DXVK待ち後のhit／compileと公開後の待ち、MoltenVK cold lock待ち、記録欠落の明示。TLSはscope終了時に前値を復元し、GCDへ必要な識別子だけscalarで伝播する。所有者情報の整合が取れないsampleはunknownに残す。既存ready-hitの追加処理を最小化する。観測した公開後待ちはscheduler等も含むので、そのままgateで削れる時間と呼ばない。

実機での性能改善は未確認。最初の保存改善も、再現済みの失敗条件への対処と診断可能性の改善であり、現在の瞬間停止解消を保証するものではない。採用する変更は独立に検証し、次の一括buildは本人のgoに従う。

## 2026-11-15 Claude初回回答を照合：保存競合を限定再現、優先gateはそのまま採用しない

Geminiとの一致点は、DXVK単体の並行fixtureだけでは下層Metalでの待ちを証明できず、必須／先読みの起点とlibrary生成由来の計測を補うこと。Claudeの「cache保存がlock分割より効く」「同じlibraryなので改善上限は約66ms」は、要求keyと由来が未計測の現状では確定しない。保存を改善しても初遭遇・非保存のmacro派生の生成時間は残る。

**保存競合:** 固定MVKPipelineCache::writeDataImplは、size queryで_dataSizeを設定した後、新libraryの公開でmarkDirtyが_dataSize=0にすると、data queryで容量0のmembufを使う。固定MVKStrings.hのmembuf／countbufと実writeDataImplを抽出し、自作数値payloadと、固定Cereal 51cbda5f30e56c801c07fe3d3aba5d7fb9e6cca4のBinaryOutputArchive::saveBinary本文を使う限定fixtureで確認した。通常40 bytesは成功、公開＋dirtyを2呼出し間に挟むとVK_INCOMPLETE=5／返却0 bytes、サイズqueryからやり直した44 bytesは成功。元bufferは変更されない。Cereal全体／実driver／iPhone実行ではなく、実メソッドの当該分岐と書出し境界の再現。公開一次資料は[Cereal固定版](https://github.com/USCiLab/cereal/blob/51cbda5f30e56c801c07fe3d3aba5d7fb9e6cca4/include/cereal/archives/binary.hpp)。

アプリ側checkpointはgetData失敗時に自身のcheckpoint行を出さずreturnする。この競合の発生可能性は確かだが、既存4ログに当該MoltenVK書出しerror文やbytes=0のcheckpointは0件、checkpoint行は短い起動3の1件のみ。実機の保存欠落原因と同定したことにはならない。改善候補はlifecycle／取消／無変更／size取得／data取得／write／renameの固定数値理由と、取消・期限・サイズ上限を保つ有界再試行。再試行ごとにsize queryから行い、継続的な生成中に成功を保証しない。新しい取消理由をゲームの保存種別と混同しない。

**ClaudeのPrio gate擬似コード:** 必須counterを残したまま、2秒を20msへ短縮した自作境界fixtureで4回のtimeout後に先読みが進むことを再現。元の定数ならwait_for予算は合計8秒相当で、compile mutex取得待ちは別。これは先読みの待ちであり、実機描画が8秒停止した測定ではない。RequiredScope destructorもp.mを取るため、別threadがp.mを保持した状態では退出できず、解放後に完了することを確認した。「必須側は待たない」とは呼べない。既に入った生成・下層lockに並ぶworker、timeoutで優先を放棄する条件と、新しいlock／通知／shutdown契約を明記する必要がある。

**WAIT_THEN_HITの撤回条件にも注意:** 現行getPipelineHandleは最初のmiss後、compile mutexを取得できるまでreadyを再検索しない。workerが要求Aを公開後、別variant Bでmutexを取り直せば、必須Aは既に完成済みでもBを待たされ得て、最終分類はWAIT_THEN_HITになる。したがってWAIT_THEN_COMPILEがほぼ0という件数だけでgateの効果なしとは判定しない。待った時間、要求origin、待機中の公開と所有者交代を合わせる。実機でこの順序が起きた頻度は未測定。

**終了通知とdevice monitor:** [AppleのapplicationWillTerminate資料](https://developer.apple.com/documentation/uikit/uiapplicationdelegate/applicationwillterminate(_:))ではbackground対応appの通常終了に通知が来る保証はない。既存checkpointは完了処理をmain queueへ戻すので、終了handlerで同じ完了を同期joinすると依存関係次第で期限まで詰まり得る。WillTerminate待ちを最初の保存改善には追加しない。MoltenVKの[上流0b90a53](https://github.com/KhronosGroup/MoltenVK/commit/0b90a53d76ccccab64fea71ecdb9554f941ecb4b)は内部MTLDeviceの生成／破棄競合に対するCTS修正としてmonitorを入れたことを確認。2021年の根拠であり現端末の必要性を独立に証明したわけではないが、今回無条件に除去する根拠もない。

限定probeはlocal/performance-20261115/claude-review/probe.py（cache_probe.cpp／gate_probe.cpp）、WSL c++で2つともexit0。exec session30162は終了済み。製品変更・cloud build・push・署名・端末操作なし。

両回答を統合した順序は、まず挙動不変の起点／wait後hit・compile／library由来／checkpoint理由計測と、独立した有界checkpoint retryを検討する。gateは修正した設計を別の実験として比較し、MoltenVK module間分離とsnapshot再設計はそれぞれ別段階に分ける。描画結果・必要な同期を保持し、コンパイル増加／先読み遅れ／初回起動時間も評価する。新しいクラウドbuildは開始していない。

## 2026-11-15 Gemini再回答の設計レビュー：計測先行・変更を分離

再回答は、PSOへのbinaryArchives追加だけでは前段のnewLibraryWithSource呼出が消えない点と、完成済みhitを長いcompile lockから分ける点を修正した。ただし提案をそのまま製品へ採用しない。追加の公開source読解と自作comparison probeのみを行い、製品コード・cloud build・push・署名・端末操作はなし。

1. **Cold要求のキーに既存matchesを流用しない。** 固定SPIRVToMSLConversionConfiguration::matchesは、変換結果のoutIsUsedByShaderがtrueのinput/output/resourceだけを比較する。既存patch_moltenvk.pyの双方向修正はdynamicBufferDescriptorsとdiscreteDescriptorSetsに限定され、未変換config全体の同値比較にはしない。実headerと実patched比較関数を使い、input binding、output location、resource msl_bufferをそれぞれ変えた3自作ケースを確認。全て未変換では左右両方向matches=true、使用フラグを立てるとfalse。結果はlocal/performance-20261115/gemini-inflight-review/probe.pyで再現（WSL c++、exit0、元constructorのclass-memaccess warningあり）。これは変換器／GPU描画の実行ではなく、比較契約の反例。待機後にreadyを正しく再検索する提案なら直ちに誤shader返却とはならないが、別要求を不必要に待たせ、狙った分離を失う。厳密な事前入力キーを別定義するか、最初はmodule別cold lockだけに限定する。
2. **macro生成は別経路。** MVKShaderModule::getMTLFunctionはpipelineCache->getShaderLibraryが返った後でlibrary->getMTLFunctionを呼ぶ。製品patch_mvk_library.pyのmacro candidate生成は_specializationLock外で行われ、cacheの_shaderCompileLockも既に解放されている。全macro生成が1本のcache compile lockで直列化されるという再回答の前提は誤り。root新規／cache読込／macro派生／競合による重複を識別しないまま、cacheロック分離が182件全ての長いMSL待ちに効くとはしない。
3. **監視対象区間の外を上限推定しない。** getMTLFunctionのdevice monitor取得待ちは、その内側のspecialize計測開始より前。newLibrary等のGCD block内のmonitor待ちはdispatch_blockへ含まれるが、別の取得箇所の上限まで12.343ms／23.326msと確定できない。単発fence_waitの最大が短くても1フレーム内の複数待ちや未計測箇所は残る。各分類の独立した最大値を引いた約885msも、個別要求に紐付けて測った区間や純粋なcompiler CPU／XPC時間ではない。MSL完了待ち優先の判断と、未測定原因の完全除外を分ける。
4. **holderの事後読みだけでは因果を確定しない。** try_lock失敗から獲得までに所有者はA→C→B等と交代し得る。獲得後のholder_req_idは解放済み0や自分を指し、別atomicのreq/originは不一致にもなり得る。所有世代と取得／解放の整合した記録、取得できない場合のunknown／複数owner扱いが必要。TLS scope終了時は0へ固定せず以前のcontextを復元する。request/originとGCDへのscalar伝播は方向として採用候補。
5. **snapshotは独立の設計変更。** configはvectorやstringを持ち、値コピーはポインタだけの浅いコピーではない。vector要素への参照を保存すれば後の再配置で無効になり得る。immutableな公開libraryの寿命とVulkan外部同期が守られる前提ならlibrary pointer参照は候補だが、全entry収集のO(n)作業・確保・memory上限を数microsecondsや100%安全とは言わない。generation付き_dataSizeだけでなくサイズqueryとdata queryの間の増加、capacity、VK_INCOMPLETE、部分payloadの妥当性、allocation failure／取消を別途検証する。現markDirtyは_dataSize=0のみで世代counterは未実装。[vkGetPipelineCacheData仕様](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html)と[destroyの外部同期](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyPipelineCache.html)を確認した。
6. **先読み譲歩は保証を弱めず設計する。** atomic urgent確認とtry_lockには競合窓があり、それだけで横取りを確実に防ぐとは言わない。再キュー＋urgent=0の即成立述語はlocal lock混雑時のbusy loopを作り得る。condition variableの通知漏れ、shutdown、worker用entryの寿命／index安定、再キュー上限とstarvationの扱いを明確にする。compiled・failed・deferredを混同しない。全shader workerを高QoSにする案は採らないが、実際のP/E配置やXPC CPU内訳は未測定。

次の最小単位は(1)既存固定量計測へ要求origin／library生成由来／競合待ちの整合した識別を加える設計、(2)その結果を見てready-hitを保ったmodule間cold待ち分離を独立検証、(3)同module別configの合流やsnapshot保存の再設計は別段階、とする。コンパイル同時数が増えることによるサービス混雑・CPU／memory／熱の悪化も比較対象。新しいdevice buildは行っていない。再回答の必要な検証欄は途中で文章が途切れているため、試験仕様が完成したとは扱わない。Claudeの独立reviewへ渡せる具体的な論点を得た。

## 2026-11-15 Gemini回答の一次資料・固定ソース照合（実装前）

対象はbuild6公開commit `6e0f5bafc879aabbbaf73d36c526d664f8296396`の経路と固定MoltenVK `458870543b9bcf6b0edd6f90aaa776707b310d96`。元MVKPipeline.mmのSHA256はpatch入力pinと一致。回答全体を実証済みとして採用せず、公開コード・一次資料・回収済み数値を区別した。製品変更、cloud build、push、署名、端末操作はなし。

- **採用価値のある指摘:** `mvk_hot_cache.cpp.in`のcold生成はVkPipelineCacheインスタンス単位の`_shaderCompileLock`で直列化される。プロセス全体で常に1個という意味ではない。現在のready-hitはこのcompile lockを取らず、同じmoduleでも別のcold生成中に利用できる。DXVK単体fixtureの独立shader組通過は、下層Metalまでの独立性を証明しない。
- **保存内容:** 固定writeDataは変換設定・result metadata・圧縮MSLを保存し、readData→addShaderLibrary→constructorは解凍してnewMTLLibraryを呼ぶ。MTLLibrary／完成PSO／macro variantsをそのまま永続化しているわけではない。OS内cacheがこの呼出を短縮する可能性は残る。起動4は304,869 bytesを入力済みなので、保存完了logなしを「cacheが0 bytes」と言い換えない。
- **device monitor:** getMTLFunctionはdeviceのObjective-C monitorを保持してfunction specialization完了を待つ。一方newLibraryWithSource側のmonitorは非同期API発行を囲み、完了待ち全体を必ず保持するわけではない。直接のmonitor取得待ちは現logで分離できず、影響は追加観測対象。

起動4の既存集計を再利用した追加比較（時間は呼出・待機を含むhost spanで、純粋なcompiler CPU時間ではない）：

| 計測 | 件数 | 最大ms | 100ms超 |
| --- | ---: | ---: | ---: |
| mvk_msl_wait | 524 | 901.934 | 182 |
| mvk_specialize_wait | 6,475 | 23.326 | 0 |
| mvk_pipeline_wait | 3,369 | 186.029 | 1 |
| mvk_dispatch_wait | 10,368 | 4.340 | 0 |
| mvk_dispatch_block | 10,368 | 12.343 | 0 |
| image_acquire | 56,330 | 3.414 | 0 |
| fence_wait | 310,037 | 44.520 | 0 |

この差はMSL生成完了待ちと、それを必須描画へ伝える共有待ちを優先する根拠。device monitor取得待ち、未計測GPU区間、guest CPUを除外する証拠ではない。990ms gapの時間重なりは有力だが、thread role／要求依存関係がないため先読み起因の完全な因果鎖とは呼ばない。QoSによる連鎖待ちは候補であり、実際のEコア配置や969msの内訳を測定したことにはならない。

[MoltenVK issue #2863](https://github.com/KhronosGroup/MoltenVK/issues/2863)は実在するが、現時点でopenの提案。別revision／M3 Pro／RPCS3で各1回の比較で、作者自身がCTSとcache保存中の並行生成を未検証と記載する。提案diffはmodule lock中にgetShaderLibrary→new MVKShaderLibraryを呼ぶため、現在の同module ready-hitをcold compile待ちに戻し得る。無修正のbackportを採用しない。lookup／publication／dirtyとserialized-sizeの整合、converter共有、merge／snapshot、同keyと異key、失敗・寿命の契約が必要。別module並列化のCPU負荷・peak memoryも未測定。

**計測提案の補正:** `shaderLibraryFromCache`はfind成功で増えるが、ディスク復元由来と同セッション内作成済みlibraryを区別せず、完成PSO命中率ではない。`queue.mtlCommandBufferExecution`はcommit前のtimestampからcompletion callbackまでで、GPU実行だけの時間ではない。累積maxを5秒窓のmaxとして差分出力もしない。標準performance trackingはdeviceの`_perfLock`を使うため、常時有効化の負荷を無視しない。frame/work枠を増やしても欠落ゼロを保証せず、roleだけで因果を100%確定しない。GCD再利用threadには仕事単位のrole／依存IDと復元が必要。

**保存区間の説明を明確化:** save_write_probeの区間はopenからではなく、最初に観測できたwrite開始からcloseまで。open前、openから初回write前、close後は含まず、write間のCPU処理・idle・並行処理は含み得る。serialization全体、manual／auto、thumbnail取得を特定できない。97件中64件に重なることを原因割合66%としたり、重複と欠落を無視して97-64-6を別原因件数としたりしない。thermal=0／headroom十分は今回の圧迫を支持しないが、全時点の周波数低下やguest heap不足を証明的に排除しない。pool回収／縮小counterは既に222観測全て0。

[AppleのQoS資料](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/PrioritizeWorkWithQoS.html)は複数lock越しのpromotionの制限を示す。[Metal binary archive資料](https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects)はspecialized functionsの端末上保存にiOS18以降を要求し、OS更新によるbinary無効化も説明する。API要件は満たせる候補だが、現在のMSL生成待ちをどこまで省けるかは生成順・関数取得・descriptor接続を含む設計と実Metal比較が必要。PSO保存だけで902ms待ちが消えるとはしない。

次はClaudeの独立reviewも合わせ、(1)最小のrole／要求依存・cache段階・欠落理由計測、(2)ready-hitを保持したcold共有待ちの縮小と必須要求優先、(3)描画を止めないsnapshot／保存条件、の順で候補を絞る。先読みyieldは開始済みcompiler callを中断しない。foreground checkpointはatomic renameだけではlatency安全や強制終了時の保存を保証しない。MTLBinaryArchiveは次段の候補に残す。

## 2026-11-15 追記：録画前からの停止と先読みの待ち構造を追加確認

本人より「録画前からかなり引っ掛かる」「地形やevent等を初めて読み込む時のように感じる」「初めてでなく時間経過後にもありそう」「まだ快適に遊べる段階ではない」と補足。録画の追加負荷があり得ることと、録画が発生に必須でないことを分ける。scene識別／cache key別hit／missは現logにないため、同じ対象の再生成・再読込みの実証とはしない。平均FPSの最大値ではなく、操作中の長いframe gapの頻度と長さを優先する。

固定DXVKの`D3D9ShaderModuleSet`、`DxvkPipelineManager`、graphics instance一覧とadapterを確認。作成済み要素のlookup／保持は存在し、この経路に経過時間による削除は見つからない。shader組と描画state／render passが同じことを条件に再利用するので、「同じ場所を見た」だけでは同一pipelineだったと証明できない。他のtexture／game objectのstreaming寿命は別であり、全cacheが消えないという主張ではない。

現行adapter後の実`getPipelineHandle`／`compilePipeline`／`createInstance`／`findInstance`と実instance classを、既存fixture generatorから取り出した。自作driverでvariant2のcompileをgateで止め、同shader組の未作成variant3、完成済みvariant1、独立shader組のvariant3を要求。Graphics／Computeの両方で、未作成variant3だけはdriverへ達せず待ち、完成済みhitと独立shader組はgate解除前に完了した。解除後は全handle一致、各variant作成1回。gateは250msの人工待ちであり、iPhoneの性能値ではない。4元入力の正規化SHA256はadapterのpinに一致。通常のbounded C++ compile／実行2種だけを行い、既存stress全体やprivate compileは繰り返していない。

実機logのcompile-lock待ちが必須描画か先読み同士かを個々に識別するには、caller roleとkey別in-flight情報が必要。修正候補は、先読みが次のvariantを取る前に必須要求を優先する方法、または異なるstateの生成待ちを分ける方法。開始済みMetal compiler callを後から中断できるとはしない。後者はpipeline生成内部の可変状態・共有cache・同key二重作成・失敗／例外・解放／終了時の所有権を証明してから採用する。単にmutexを除く変更はしない。同じvariant自体の初回コンパイル時間は、この分離だけで消えるわけではない。

新probeのsource／結果は`local/performance-20261115/pipeline-wait-probe/probe.py`／`result.json`。今回も製品変更・Mac build・push・署名・端末更新は0。本人はUSB取り外しを希望し、PCに保存済み4ログで続行できることを伝えた。端末通信は終了済み、直近1ログは次の接続時に終了状態を確認して回収する。

## 2026-11-15 01:10：build6の実機stutter報告と終了済み4起動の解析

本人は画質を変え、一部で画面録画も使ったと報告。「前より増えた」は比較仮説として保持し、録画だけのせいにも、build6の回帰確定にもしていない。現在のappは画面録画のオン／オフを記録していない。起動時のresolution overrideと項目数は残るが、全画質tuple・同じscene・録画有無は復元不能。`IOS_PERF`の平均FPSだけでは瞬間停止を評価しない。

既存USB信頼のみでbuild6を確認。更新完了後5起動のcatalogを取得し、直近起動は新しい保存・終了確認待ちとした。それ以前の4ログは取得前後のsize／mtime／birthtime不変、計1,759,588 bytes。端末書込み・元game／save／IPA変更なし。Rootローカルの`private/iphone-build6-20261115-0102/collection-prior.json`、`analysis-01.json`〜`analysis-04.json`、`temporal-prior.json`に対応を保持。既存`analyze-frame-timing.py`を使用し、invalid／duplicate timing recordは各0。過去の約4.44GBログは再取得しない。

| 起動 | 計測区間（約） | 起動時解像度override | 追加cap | 100ms超のframe gap | 最大gap |
| --- | ---: | --- | --- | ---: | ---: |
| 1 | 6分15秒 | 956×440 | 60 | 63 | 18.879秒 |
| 2 | 3分45秒 | 782×360 | 60／0 | 10 | 0.926秒 |
| 3 | 15秒 | 782×360 | 60／0 | 0 | 0.056秒 |
| 4 | 19分03秒 | 956×440 | 60 | 100 | 7.187秒 |

これはlogのmonotonic timing区間で、全てを操作中gameplayとは呼ばない。startup／menu／loadingを含み、解像度overrideはswapchain寸法や他の画質設定を表さない。区間境界にかかる5秒histogramはend時刻で分類した。起動4では0〜120秒に9、120〜600秒に64、それ以降にも27の100ms超gapがある。10分以降にも残るため「最初だけ待てば必ず解決」とは言えない。

### 描画生成と待ち

起動2は100ms超のsampled gap10件中9件がgraphics pipeline／compile-lock／Metal MSL生成のいずれかと重なった。起動4はsampled97件中64件（histogramの100件全てをsampledできてはいない）。起動4のsamplingはbusyで1件、固定容量で294件を落としており、重なりのないsampleも原因なしとは扱わない。各種のnested／並行時間は加算しない。

起動4の約751.942秒からのgap990.256msでは、同threadのPresent973.729ms／submission待ち973.672ms、別threadのgraphics compile-lock待ちの区間和968.562ms／MSL待ちの区間和903.496msが重なる。約772.821秒にもgap369.107ms、同threadのsubmission待ち362.984msと別threadのcompile-lock待ち350.308msがある。別threadのoverlapだけで因果を断定しないが、描画側の実際の待ちも観測しており、必須描画に必要なpipelineの準備待ちが有力。`prepare_dxvk.py`では未作成variantがshader tuple単位のcompile mutexを取り、先読み側も同じmutexを使う。計測はmutex取得待ちと実vkCreateGraphicsPipelinesを分離している。旧build5からの待ち方の全回帰を証明したものではない。

起動4のD3D shader-create自体は最大0.692ms、shader-moduleは1.746ms、ReadFileは7.56ms、ReadFileExは1.46ms。現ログからguest shader生成関数やread syscallを主犯として全面置換する優先度は低い。ただしguest側のscene準備／serializationなど未計測時間を除外したことにはならない。

### cache・保存・温度

- Vulkan cache入力は起動1〜3が0 bytes、起動3で304,869 bytesの保存成功（6.618ms）、起動4で同bytesをdriverへ渡した。cache hit数は不明。DXVK state cacheは別系統で、起動1は2,389 entries／679 shader tuples、起動4は2,706／701を読み、compiler workerは3。Vulkan cacheが全起動で壊れている、逆に十分warmである、のどちらとも断定しない。製品codeのcheckpointはbackground遷移時のみで、今回の起動1／2／4には完了記録がない。再利用が十分成立しない可能性は調査候補。強制終了・取消・未実行・取得失敗を現logだけで区別できない。
- 起動4のsave候補handle区間は7回（最大539.730ms）。6回が338.031〜605.118msのsampled gapと重なる。write syscall406回は最大0.696ms、総和8.781msであり、file-writeだけで数百ms停止を説明できない。handle区間はidleを含み、保存前のserializationと後のdurabilityは含まない。manual／autoの判別もできない。autosaveを勝手に無効化せず、save-preparation側の候補として保持。
- 起動4は222観測全てthermal=0／low_power=0／foreground=1／memory_warning=0。OSの追加headroomは最小4,013.326MiB（約3.92GiB）。pool回収・縮小counterも全観測0。これは内部guest heapの余裕や温度／GPU utilizationの直接測定ではない。起動1／2にはthermal=1もあるが2／3はない。今回の停止を重度thermal／OSメモリ圧迫だけで説明する証拠はない。
- 前回のdescriptor pool不足に伴う大量warningは、今回の終了済み4ログで該当0。これは実際のlog側の改善であり、FPS改善率の証明ではない。復帰時GPU gateはbuild6から撤回済みなので今回の原因候補へ混ぜない。

### 次の切り分けと制限

まず直近1ログを終了確認後に回収し、録画していない同save／同画質の短い走行を必要になった時だけ比較する。修正候補は(1)cacheの保存／再利用を実際に成立させる経路と理由の計測、(2)必須variantが先読み中の別variantを待つ経路の分離／優先制御、(3)保存準備区間。threadを闇雲に増やす、未完成描画を飛ばす、syncを外す変更は採らない。今回変更されたfloat互換経路もcold compileのkey／生成コストを調べる対象だが、因果未確認のまま描画修正を戻さない。画質・scene・録画・cache状態が異なるため、build5の部分prefixとbuild6の全logから回帰率や改善率を計算しない。新しい製品実装／テスト／Mac build／push／署名／端末更新は行っていない。

## 2026-11-14 build6：Mac最終検証と端末更新を完了

[Mac build20](https://codemagic.io/app/6ad977fcf240708df6941076/build/6af873d290e83b09c4fa38fc)、公開commit `6e0f5bafc879aabbbaf73d36c526d664f8296396`で成功。実ログは`local/codemagic-title20/build.log`、個別Metal結果は同じ場所の`metal-alias-checks.zip`。34組のunit fixtureは計82件中80成功、2件はWindows固有cross-compilerと未構成host FFmpegの既定skip。Mac実行に必須の検査をskip扱いにはしていない。iPhoneOS Metal shader8件が全てcompileし、さらにMac GPUで実行時入力を使う20個のfast／safe数値を読戻して独立期待値と照合した。今回のcomponent導入後は最初の再compileで準備完了となった。

元MoltenVK submission4ファイル、旧gateの非link、library寿命／圧縮payload共有／compile計測も成功。撤回したgateの失敗reproは残す。CPU／関数table／公開guest ABIの3headerは既存build5と同一であり、未変更private guestをPC内だけで組み込んだ。署名後に2images・39,426pages・4resourcesとprofileを確認し、本人の準備完了後に既存USB trustによる5→6のUpgradeを完了。ゲーム26ファイルの名前・サイズ、セーブと設定を含む11profileファイルのSHA256を前後照合した。旧game／IPAは保持。

これは移植アプリの生成・署名・更新と自作描画fixtureの合格であり、更新版でのゲームプレイ、FPS、熱、水中や透過表示の改善証明ではない。ホーム復帰時のdevice lossも未解決。実機比較は同save／同画質を基本とし、初回shader生成とwarm再訪、魔法／視点操作、水中、UI操作を分けて確認する。Free枠の最終表示は193/500分（残307分）。今の更新のための追加Mac起動は不要。

## 2026-11-14 Mac build19：Metal Toolchain準備の有界待機

休止gateを除いた公開`188afae`のMac検証で、元submission4ファイル、library／compile計測fixture、ARM64 app linkは成功。公式`xcodebuild -downloadComponent metalToolchain`のexit0直後にも最初のcompileがmissing Metal Toolchainを返し、その後の7個は全てexit0だった。全8個成功を要件としていたためpackage公開を拒否した。数値GPU20ケースはこの失敗より後なので未実行。rawは`local/codemagic-title19/build-step4-visible.log`。

製品を変えず、コンポーネント導入後の実compileで、そのmissing応答だけ最大11試行／sleep合計20秒の待機を認める。別のshaderエラーと出力欠落は即座に失敗し、導入コマンドは1回、全試行の別logを保持する。実shell関数をそのまま使い、即準備完了／2回遅延／永続missing／shaderエラー／遅延後shaderエラー／成功exitだが出力なしの6状態を自作toolで検証した。2検査成功（0.348秒）。実Apple toolの再検証は次の一括Macに残す。完成条件・描画・private guest・署名ABIを変更していない。

## 2026-11-14 Mac build18：GPU休止gateを実機構テストで棄却

[Codemagic build18](https://codemagic.io/app/6ad977fcf240708df6941076/build/6af86f0990e83b09c4fa3754)（公開`26348ab`、Apple Clang21／Xcode26.6）で、既存のhost fixtureとMetal library fixtureは成功した。しかし実Metalのevent依存テストで、gateを閉じた後の`waitUntilScheduled`が5,005.529ms待ち、consumer／producerにerrorがないというassertが失敗した。未投入producerがgate復帰を待ち、既投入consumerがそのeventを待つ条件である。これを「すべてのiPhoneで同じ停止が起きる」とはしないが、普遍的に安全な休止処理という採用条件を満たさない。ARM64 app compile/linkは成功、必須検査が失敗したため配布packageは作らなかった。

今回のbuild6から`patch_mvk_lifecycle.py`の適用、`gpu_gate.c`／`gpu_gate_metal.m`のlink、UIKitとevent pumpからの呼出を撤回した。元の固定MoltenVK submission4ファイルをbyte hashで検証する`verify_lifecycle_baseline.py`を追加し、旧stage／archive／最終appにgateが混入した場合は拒否する。既存performance activity観測と他の性能・UI・描画変更は保持。dependency stageは新規`ios-dependencies-build6`。private guest、CPU ABI、ゲームデータは不変。

失敗を隠すためのassert削除／timeout延長は行わない。候補と同じ実Metalテストは`experiments/gpu_lifecycle_gate_test.py`で再現可能な形に保管し、本番から除いた候補の実行をrelease gateに要求しない。代わりに元submission4ファイルと未接続を確認する3検査を製品routeへ追加した。Clang18で関連21件中20成功・Apple専用1skip（3.749秒）、shell3本syntax成功。Metal shader compile／数値readbackを含む他の必須gateは維持し、次のMacで確認する。

背景移行時のdevice lossは未解決として残す。将来の候補には、main-threadを待たせないquiescence、未投入producer／先行enqueue／別queue eventの依存解消、復帰と取消の寿命を含む設計が必要。今回の局所fixture成功やcompile成功をFPS・発熱・水中表示の改善と扱わない。失敗原文はRootの`local/codemagic-title18/build-step4-visible.log`。

## 2026-11-14 夜：派生Metal libraryの圧縮payload保持を削減

固定MoltenVKではmacro派生libraryは再特殊化せず、serialized root cacheの要素でもない。各派生が親と同じ圧縮MSLをcopyして保持する必要がないため、`patch_mvk_library.py`で同期decompress中だけ親のimmutable payloadを参照する。生成したstringとMetal compilerの所有権は既存通り。rootはcache入出力と後続variant用のpayloadを保持し、wire format／shader／compile回数／map lock／失敗方針／期限／guest ABIは変えない。non-constなupstream codecをconst_castせず、固定codec headerの同一decoder式を使う。

実methodと固定codecを使う自作Metal境界で0/1/21/16,384/1,048,576 bytesの入力を比較。最大ケース16 variantsで派生側の重複保持16,777,216→0 bytes。root bytes、decoded shader、寿命／copy／assignment／例外／並行publish、8,000 cache hitsが一致。通常3検査19.795秒、ASan/UBSan3検査53.206秒で成功。前のsanitizer処理はアプリ再起動で最終結果が読めず、結果不明を合格とせず1回だけ再検証した。この値は自作条件の保持量で、実アプリRAMやFPSの改善値ではない。Apple SDK compile／実Metalは一括buildで確認する。patchのhashは既存dependency cache identityへ直接含まれる。

macro singleflight試作は製品に接続しない。条件変数待ち／再入／別lockの待ち／Apple compiler deadlineの実経路を十分に証明できず、timeout後の重複生成も最悪待ち時間を保証しないため。今回の削減でcold compileがなくなったとはしない。

内部heapは元のlock付き56-byte snapshotと、詳細走査を省く引数を確認した。ただし元lock取得は無期限のretryを持つ。独立monitor threadから呼ぶ設計を採らず、既存lock所有中の低頻度observer／破棄世代／全writer／CPU ABIの確認を次の接続条件にした。動的used・high-water・不足頻度は未測定、増量なし。private解析はRoot内の既存friend-heap記録へ保存。MOD管理は別設計書にまとめ、今回の実装に混ぜない。

## 2026-11-14 夜：オートセーブの選択を実装／SPOの実差分を判定

### オートセーブ設定

`mobile_ui.m`のSettings → SavingにKeep current／Off／Onを追加した。English既定・Languageの日本語にも対応し、既定／不正値は維持。選択はstartup snapshotへ固定し、次回起動時だけ`SkyrimPrefs.ini`の`[MAIN]`にある`bSaveOnPause`／`bSaveOnTravel`／`bSaveOnWait`／`bSaveOnRest`へ0／1を適用する。Keep currentは現在値を保ち、以前のOffを元へ戻す操作ではない。ゲーム中にゲーム側メニューで変更することは妨げない。Off中の手動保存と再起動の必要性を画面に記載した。

4項目の綴りだけでなく、固定所有入力のsetting登録先とsection解決をRootで確認した。公開SKSEのINIPref collection定義とも一致。解析命令・アドレスはprivate内のみ。変更は公開hostのみでprivate guest／ABIは変えず、現在の端末や実プロフィールには適用していない。manual／F5 save、`fAutosaveEveryXMins`、画質preset、FPS capは変更対象外。

実staged `skyrim_shims.c`からINI parser／writer／seederをそのまま使った自作fixtureで、既存mixed-case設定のオン／オフ、欠落keyの追加、Keep current／不正値でのfileバイト不変、再起動相当のcache破棄後の読戻し、初回presetと4項目の優先順を確認した。画質／VSync／timer／言語／archive／同名別sectionは保持。通常とASan/UBSanの1ケース、既存mobile overlay 5ケース成功（計6 unique、sanitizerは同ケースの別実行）。次回Macルートにも追加済み。Apple SDK／UIKit実行、実機での保存発生・停止改善は未確認。保存と無関係なcamera／magicの停止がこれだけで消えるとはしない。

### SPO：依存は適合、元配布版の一括採用は見送り

本人のログイン後、[作者のclassic配布](https://www.nexusmods.com/skyrim/mods/32505?tab=files)から本体のみ1.6（file id `1000102231`）を無料の通常downloadで取得。RARは72,507 bytes、SHA256 `7844dad6985a34f37c4fcdc45cb9c646c1fd39cd61f78b8e902295093e2df882`。`local/friend-review-20261114/spo-classic-1.6/`へ隔離展開した。原Data／load order／セーブ／端末は変更していない。

作者同梱説明はESMを既定、ESPを問題時の代替としており、両方同時には使用しない。ESMのmasterはSkyrim.esm／Update.esmで現在構成に適合。145 CELLは全て室内、REFRは735。所有Skyrim.esm→Update.esmの順で上書き済みbaselineを作り、group境界／圧縮payload／subrecord境界を確認して比較した。fieldの意味は[xEdit公式TES5定義](https://raw.githubusercontent.com/TES5Edit/TES5Edit/dev-4.1.6/Core/wbDefinitionsTES5.pas)を参照。

- 新規REFRは684。そのうち677はPlaneMarkerを参照し、残り7は壁材／家具／容器等。既存REFR 51件にも差分があり、50件の位置・回転DATAが異なる。うち49件は位置の最大成分差が1 game unitを超える（位置だけの実差、見た目や不具合の証明ではない）。
- CELLのFULLはrawでは145件異なるが、所有English STRINGSを解決すると143件は同名、2件は実名差分。全145件を表示変更と誤判定しない。一方でXCLL／image space／location／所有者／音楽／region／encounter zone等にも実データ差分がある。
- 水面高54差分のうち、before／afterのいずれかでHas Waterが立つのは1件。残りを水の見た目変更と断定しない。既存の水中茶色問題の対策と混同しない。
- 任意ESPも同じ件数だがESMと完全同内容ではない。自己recordのindexを比較用に対応付けた後も7件にXOCP／XPRM／DATA差分が残る。無条件の代替互換と扱わない。

**判断：SPOの遮蔽による処理削減は引き続き検討する価値があるが、この元配布版を「描画・挙動不変の最適化」として丸ごと導入しない。** 次段は遮蔽markerと既存配置の依存を室内1箇所ずつ確認し、必要なら隔離した比較候補を作る。物体の移動を除けば自動的に同じ見た目になるとは限らない。実機の視点移動・door／可動物・透過物・反射の確認前に採用しない。これは元MODの不具合認定ではなく、本プロジェクトの品質保持条件に対する保留である。私有比較の詳細は`private/analysis/friend-heap-20261114/spo-*.json`。

### 内部heap

追加でdefault heap constructorから初期領域取得まで4入口の限定命令を確認した。初期パラメータは予約量とcommit量を分けてvirtual methodへ渡す。固定の容量を増やせば直ちに同量の物理RAMを使えるわけではなく、使用量／high-waterと同義でもない。全alloc／free／growth writerの契約と実機pressureは未確認。Sheson相当の容量変更、allocator置換、SKSE DLL導入は引き続き行わない。

## 2026-11-14 友人案の実施開始：保存計測と適合性の確認

本人の「計画立てして取り組んで」を受け、[3段階の実施計画](FRIEND_OPTIMIZATION_PLAN.md)を作成した。新GOまでアプリ／private guest／クラウドbuild、push、署名、端末更新は保留。既存build5とgame／save／設定を維持する。

- **保存I/Oの計測をpublic hostへ追加。** WriteFile／WriteFileExの実syscallを既存の単調時計・5秒histogram・50ms以上の固定量spanへ接続した。通常writeと、末尾が`.ess`／`.ess.tmp`／`.ess.bak`のsave候補を区別する。新しいログへファイル名・内容を出さず、stdout／stderrは対象外。分類はopen時に一度だけ行い、記録用heap割当・ファイルI/O・新mutex・fsync・write再試行は加えない。
- **候補handleの経過区間も追加。** 最初に記録できたwriteからcloseまでを観測し、複数の短いwrite間の時間を残す。ただし保存前のserializationや後のrename／永続化は含まれず、idleや別threadも含む。autosaveの識別でもCPU時間でもない。threadは0、解析器は`handle_lifetime`と表示し、closeしたthreadへ帰属させない。既存kind番号は維持して末尾に5分類を追加。
- **元の動作の保持を確認。** 観測を取り除くと元sourceに完全一致。実shimを使い、正常／部分／0byte／失敗／無効handle／stdout／stderr／read併用／APCの順序・結果・offset・errnoを旧新比較した。write中にcloseが進む場合も観測がGFileの寿命を延ばさないよう、分類値をsyscall前にコピーし、戻った後にfile objectを再参照しない。自作4thread×1,000回の記録、境界filename、handle再利用も確認。
- **検証範囲。** 新規3検査は通常とASan/UBSanで成功。既存asset timing4、histogram／stall5、解析器7、mobile overlay／CPU header5、Metal分類番号1が成功（重複除外25件）。固定runtimeの全patch合成と実kernel32.cのWSL syntaxも成功。既存Metal待ちfixtureはこの実行ではsource環境変数なしでskipし、成功に数えていない。Apple compile／実Metal／端末の保存比較は未実施。無料Mac枠は使っていない。
- **SPOの候補を特定。** 内蔵ブラウザの作者FILESでclassic1.6の本体のみ版`Skyrim Project Optimization`（71KB、file id1000102231）とNo Homes版を確認した。Full VersionはDLCを含むため現在の構成へそのまま選ばない。配布画面は未ログインで取得不可。認証制限を回避せず、実ESPのmaster／CELL／REFR差分と実機表示比較は未確認として保留。
- **内部heapは静的照合まで進めた。** [公式SKSE](https://skse.silverlock.org/) classic1.7.3配布のsourceだけを隔離展開。容量を変更する2位置は固定所有入力の命令境界／即値幅と一致した。5入口・451個の異なる命令位置をRoot内で限定確認し、TLS別pool、複数allocator、失敗時の回復／再試行、reserveとcommitの区別を記録。live pool使用量・全field／lock契約は未確定で、容量変更やnative hookは行っていない。詳細はRoot専用`private/analysis/friend-heap-20261114/`。public SKSE sourceもalignment引数は未検証と明記しており、一括置換の根拠にしない。

今回の成果は原因を切り分ける計測と、適合する候補／接続条件の特定。FPS向上・瞬間停止減少を実機で証明した変更ではない。既存のpipeline／描画待ちの対策候補も維持する。SPOは正規のファイル取得後に依存・差分を確認し、保存ON/OFFと同時には比較しない。

## 2026-11-14 友人のPC版対策案：未実装の採否調査

本人の「真剣に調査、まだ実装しない」に従い、公開一次資料・現在のsource・既存端末ログ／導入時の一覧を読み取りだけで照合した。MODのdownload／導入、INI・セーブ・設定・runtime・guestの変更、端末接続、build、push、署名は行っていない。以下の採否は次の実験候補の判定で、効果を実機実証した結果ではない。

### 判断

| 提案 | 目的と今回の判断 | 採用前に必要な証拠 |
| --- | --- | --- |
| オートセーブ停止 | 保存時の停止を切り分ける低コストの比較候補。恒久的な既定OFFは採らない | 同場面で保存処理／保存fileの更新時刻とframe gapを対応付ける。設定以外を固定し、手動保存を保つ |
| SKSE一式の導入 | mod向け拡張基盤であり、導入だけで全体が速くなる仕組みではない。今回のstutter対策として一括移植の優先度は低い | 必要な特定機能と費用を示す。Windows loader／DLL／x86 code patchが現在のAOTへそのまま適用されるとは扱わない |
| Sheson由来のheap調整 | ゲーム内の特定poolが不足する場合は意味がある。現状は測定候補で、拡大は保留 | pool別high-water／限度／失敗／拡張待ちと停止の対応、guest側のアドレス余裕。PC定番数値の流用はしない |
| Skyrim Project Optimization（SPO） | 見えない室内objectの描画を減らす原理は有望。選択された室内での比較候補として優先 | classic版・実master依存・load order、同じ視点での欠け／透け／反射／光の不変、draw／CPU／GPU／frame gapの比較。元から見えていない物が実際に除外されたかを測る |
| SkyrimPrefs.ini調整 | 具体keyごとの比較には価値がある。現UIにも対応あり。一般的な「INIを変える」だけでは最適化案にならない | 実際に読まれるkey、優先順位、ゲーム結果と画質への影響を確認。影／距離／mipを下げた差を品質不変の効率化と数えない |
| Increased Memory Limit | 既存署名設定にあり、対応端末での拡張を要求済み。今回の追加実装ではない。無条件RAM制限解除は不可 | 実OS予算とfootprint／warningを観測。少RAM機で増加するという保証を置かない |
| 30／60／上限なし | 追加上限0／24／30／40／60を実装済み。ログにもcap=30／60あり。今回は追加不要 | 上限なしは追加capなしで、VSync等は残る。熱／長時間の安定は別測定。比較中はcapを固定 |

### 新しく照合した実機数値

既に回収したbuild5の約1,043秒prefixを使用。4.44GB全ログを再走査していない。`IOS_DEVICE_STATE` 205行はいずれもheadroom_known=1で、headroomは最小 **4,886,345,512 bytes（約4.55 GiB）**、最大6,431,472,360 bytes。memory_warningの正値は205観測中0。`device_platform.m`により、値は`os_proc_available_memory()`の戻り値であることを確認した。[AppleのAPI定義](https://developer.apple.com/documentation/os/os_proc_available_memory)では現在のアプリ上限とfootprintとの差の目安であり、物理空きRAMでも永続保証でもない。

この観測点ではiOSのアプリメモリ上限に詰まっている根拠が弱い。観測間のpeakや未回収区間、ゲーム内部pool不足まで否定しない。memory_warningは内部の期限付きpressure状態であり、全期間の通知回数0を証明するカウンターでもない。

同prefixの`IOS_PERF`はcap=0が8行、30が178行、60が20行。行数は秒数やフレーム数の割合ではない。FPS上限が切り替わっており、全区間平均を同条件の性能比較に流用しない。既存集計のgraphics pipeline最大571.642ms／100ms超220回は保持するが、保存由来の停止と直接対応する時刻情報がないため、全stutterをshaderまたはautosaveの一方へ帰属させない。

### オートセーブ：根拠と限界

[Bethesdaの旧Xbox 360向けサポート](https://help.bethesda.net/app/answers/detail/a_id/9925/~/what-should-i-do-if-the-elder-scrolls-v%3A-skyrim-is-freezing-%2F-crashing-on-xbox)には、freezeの切り分けでautosaveを無効にして比較する案がある。ただしこれは今回のPC1.9.32／iOS互換層の因果証拠ではない。[保存中にSave／Loadが無効になる説明](https://help.bethesda.net/app/answers/detail/a_id/9657/~/i-am-unable-to-save%2Fload-because-the-option-is-greyed-out.-what-should-i-do%3F)とも区別する。2011版のすべての瞬間停止がsave system由来という一般化はしない。

回収済み導入前profileの2INIにautosave関連keyは見つからなかったが、これは設定OFFの証拠ではない。現在の計測はread／texture／pipeline等が中心で、セーブのserialization全体やwriteに専用spanがない。公開runtimeのWriteFileはhost writeを呼ぶ。FlushFileBuffersは現在成功を返すだけで、独立したfsyncを実行していないため、元Windows版のディスクflush待ちをそのまま今回の停止原因にしない。保存の永続性の互換契約は別課題であり、この調査で変更していない。

比較するなら同じセーブ、同じcap／画質、warm cache、同じ移動／menu／戦闘操作を使い、保存の有無と時刻を明記する。保存時だけ停止が減れば保存処理が候補、保存していないcamera／magic場面で残れば別経路を追う。無効化による進捗保護の減少を、性能修正と同一視しない。

### SKSE／Sheson：3種類のメモリを分ける

[SKSE公式更新履歴1.7.0](https://skse.silverlock.org/skse_whatsnew.txt)に、Shesonへの謝辞と初期pool size設定の追加がある。[Sheson作のMemory Blocks Log](https://www.nexusmods.com/skyrim/mods/50471)もpool別使用量を調べる道具で、作者の[説明](https://www.nexusmods.com/skyrim/mods/50471?tab=posts)は上限へ達するかを見て設定する方針。設定値を増やせば必ず速くなるという根拠ではない。

今回分けるのは、(1)Skyrim内部pool、(2)互換runtimeの32bit address layout／allocator、(3)iOSのアプリ全体予算。固定`rt.h`は1GiBのguest heap address帯と2GiBのVirtualAlloc帯を持つ。これらは確保済み物理RAMではなく、Skyrim内部poolの空き量でもない。Apple側の余裕が4.55GiBあっても内部poolが空いているとは言えない一方、内部poolの拡大がApple上限を拡張するわけでもない。

vendorにはSKSEの独立した部分Papyrus互換sourceがあるが、今回のiOS CMakeは`skse_*.c`を除外している。完全SKSE、Windows DLL loader、Sheson patchが導入済みという意味にはならない。[公式SKSEの導入方法](https://skse.silverlock.org/skse_readme.txt)はWindows loader／DLLを用いる。現guestは既にARM64へ事前変換されているため、元x86 code bytesへのruntime patchを置くだけでは生成済み関数に自動反映されない。必要なら限定した同等処理を移植し、元の割当失敗／初期化／所有権／32bit範囲を維持する。その前にpool不足の根拠を得る。

### SPO：画質不変の原理は有望、対象と互換性は限定する

[classic版の作者説明](https://www.nexusmods.com/skyrim/mods/32505)は、室内にocclusion planesを置き、壁や床の向こうに隠れた部分を描画対象から除くもの。主にcellデータの変更であり、SKSE DLLの移植とは異なる。適切に動けばdraw提出やGPU処理が減るため、今回のCPU変換／描画bridgeにも利益があり得る。これは推論であり、本移植での測定値ではない。初回の必要PSO生成や屋外全体を直接解決する手段ではない。

導入済みgame file一覧のmasterはSkyrim.esmとUpdate.esmだけ。SE／AE向けやDLC全部入りをそのまま選ばず、classic版の選択ファイルのmasterを照合する必要がある。作者はcell編集が他の照明／fog設定を上書きし得ること、配置変更との相性を説明している。見える物が消えるなら採用失敗であり、軽量化とは数えない。現bridgeにはquery API、DXVKにはocclusion query処理があるが、これだけでSPOのplane cullingが正しく動く証明にはならない。MOD本体を取得／解析／実行していないため、全依存・全cell差分・視覚一致は未確認。

### 次に行うなら

次回の許可後に、まずautosaveと停止の時刻対応を比較し、別枠でclassic／base-gameに合うSPOの1室を同条件比較する。内部heapはhigh-water／失敗を調べてからSheson相当の調整を選ぶ。現在の主な停止候補であるpipeline生成／待機の分析も維持する。INIまとめ変更や同時MOD投入は原因を分からなくするため行わない。友人には、具体的な提案への謝意、SPOと保存処理を検証対象にすること、FPS／署名メモリ権限は既存であること、SKSEは必要な部分の移植を検討することを伝える。

## 2026-11-14 Claude追加レビュー：変換器段階と互換性の前提を補完

[段階移行設計](NATIVE_MIGRATION.md)に、変換器の既存機能／新候補、ABI適用の証明、FPU oracle、TSO分類、leaf切替／shadow実行、SEH境界、3試作の追加条件、排他的コスト帰属表を追記した。Rootで固定sourceと既存fixtureを確認した設計変更で、production変更・新性能実測ではない。

新しく確認した根拠は、(a)固定liftのGPR local／direct call／site別target cache、(b)call flags patchと自作8種の回帰契約、(c)固定DXVKのSetupFPU ARM未対応分岐と既存build5 prefixの対応警告1件、(d)guest CPUごとのCW初期化はあるが通常x87演算がdoubleのままであること、(e)managed eviction optionの既定falseと後続Lockのreadback／wait経路。公開source参照は`tools/lift.py::emit/call_direct/lift_terminator/lift_x87`、`runtime/x86.h`／`kernel32.c`／`d3d9_bridge.cpp`、固定DXVK `d3d9_device.cpp::SetupFPU/LockImage/UnlockImage`と`d3d9_options.cpp`。

既存端末prefixだけを読み、4.44GB全ログの再走査や端末再接続はしていない。FPU警告は実行された分岐の根拠であり、stutter／水の原因と断定しない。回収済みhistogramからguest／DXVK／MoltenVK／waitの排他的割合は作れないため未取得と記載した。未変更のfixtureを再実行せず、docsの差分・公開source検査だけを行う。新GOまでアプリ／guest／クラウドbuild、push、署名、更新を保留。

## 2026-11-14 追加3件：通知境界・payload view・macro生成共有

直前のstate-cache重複排除を維持し、次の3件をlocalで調査した。端末なし／新GOなしのため、アプリ・private guest・Apple／cloud build、push、署名、更新は行っていない。ここでの「完了」は限定解析・試作の比較・採否の記録までであり、3件の製品導入ではない。

### 1. 完了待ちの寿命を追加解析：通知置換は保留

元入力の既存SHA256を照合し、周辺25入口＋owner9入口＋constructor／callback4入口を限定確認。全てが性能候補ではなく、無関係なfile helperを含む範囲確認なので「38件最適化」等には数えない。vtable即値のraw参照も元PEから照合した。詳細・命令・アドレスはRoot専用`private/analysis/stutter-goal-20261114-01/`にのみ保持。

requestは複数のwrapperへ埋め込まれ、constructorでidleへ初期化される。readだけでなくwrite受理後もpendingになり、元のAPC callback以外にもvirtual callback後にcompleteを書くentryがある。結果を格納するconcrete callbackとwaitによるidleへの消費も確認した。destructorにはvtable切替／解放だけでpending要求をdrainしない経路があり、上位caller側の利用終了保証を確認する必要がある。

CancelIo系の静的importはないが、取消が存在しない証明にはならない。間接call、alias経由の書込み、上位の再使用／解放規約が未確定。ReadFileExだけをhookして通知に変える案は被覆不足で不採用。次は限定した実行回数／待機mode／yield数の観測を先に行い、利用される要求世代とquiescenceを特定してからhost wait recordを接続する。ABIや待機動作をこの調査で変更していない。

### 2. texture payload view：コピー削減は成立、loader統合は保留

`experiments/dds-view/dds_view.h`と`tests/test_dds_view.py`を追加。調整した128-byte headerを所有し、保持するpayloadは元bufferから借用する明示的な型で、flat DDSのふりをしない。完全なpower-of-two／2D／DXT1〜5／範囲内mipに限定し、全宣言mipの範囲を確認してから出力する。不完全・cube／volume・未確認layoutは出力を変えず拒否。通常loaderへのfallbackをする場合も副作用前だけとする。

実公開DDS parser／level関数と、自作の独立した連続DDS生成を比較。5形式、長方形、1pixelまでのtail、全skip位置、truncation、余剰末尾、拒否時の出力不変、独立stagingへのコピー後に元bufferを解放する順を含む2,031条件が通常／ASan/UBSanで成功した。全保持mipのbyte列・寸法・pitch／rows・formatとheaderが一致。4K BC1で先頭mipを除く例はpayload再コピー **2,796,216 bytes→0**、headerコピー128 bytesを維持する。GPU upload自体は省いていない。

実際のguest allocator／free副作用、D3DX Create／Lock／Unlock失敗、wrapper公開、Metalのpixel結果は未検証。fixtureにGPUがなく、byte一致を実GPUのpixel合格とはしていない。借用元の同期寿命がhost統合時に必要で、非同期uploadは独立stagingを所有する。ゲーム関数や本番hostへ未接続。単に画質を下げた結果ではなく、同じ選択済みmipを別の入力表現で扱う試作。

### 3. macro variantの同時生成共有：8→1の試作、deadline条件は未達

`experiments/mvk-flight/`と`tests/test_mvk_flight.py`。既存の固定公開copy／assignment／destructor／constructor／macro key処理を使い、lookup区間だけper-key pending recordへ置換して比較。Metal API境界は自作。現production patchでは8同時cold要求を8回生成して1個だけ公開するが、試作では7followersが待機へ入ったことも確認し、生成 **8→1**／単一公開を維持。warm keyと別keyはpending mapの短いlockだけを通り、先行compileの完了前に取得できる。8thread×1,000 warm lookupも確認。

失敗libraryのcache方針、copy元破棄後の所有権、assignment／自己代入、key順序・size・無関係項目は既存比較を継承。例外時は同じflightの8呼出しへ例外を渡し、pendingを除去し、明示的な次回呼出しで再試行できる。native compile中の同期再入は独立生成へfallbackし、同threadの自己待ちやnested keyの待ちを追加しない。通常3検査／ASan/UBSan3検査を通過。ASan/UBSanはrace検出や実Metal動作の証明ではない。

**本番採用しない理由:** 現MoltenVKにはGCDへのdispatchと`metalCompileTimeout`があり、実device monitorとdebug/error callbackの推移的依存もある。試作のcondition waitには独立した期限がなく、thread_local再入検知もGCD先へ伝わらない。元のtimeout・late completion・owner errorを維持したwait admissionが未完成なので、数値がよくても`patch_mvk_library.py`へ入れない。通常の非再入fixtureの8→1を実deviceの全経路へ一般化しない。

次に進める条件は、同一variantの実同時missを観測し、完全なkey／device・owner寿命、leader deadlineとfollowersの失敗規約、late callback、既存lock順序／再入を一つの契約にすること。短い任意timeoutで二度compileしたり、timeoutを延長・無効化して見かけ上成功させない。実Apple SDK／Metalでの確認は新GO後にまとめる。

### 長期方針と残作業

Claudeの全面復元案は[NATIVE_MIGRATION.md](NATIVE_MIGRATION.md)へ一次資料と採否を整理した。既にARM64で動く現baselineを維持し、hostの重複処理→guest ABIを保ったnative leaf→所有権が明確なcomponentの順に進める。matching元x86とARM64の挙動一致を分け、全領域の復元を待たずに改善する。今回の3件は性能仮説・処理量・安全条件までを確定し、実機FPSや熱の改善率は未確認のまま残す。

## 2026-11-14 午後：①〜③の深掘りと先読みjobの重複排除

本人のgoal指定に従い、shader／素材、texture準備、read／完了待ちの3系統を追加解析した。新たに21入口を確認し、所有権・thread／TLS・失敗・callback・置換条件をRoot専用の`private/analysis/stutter-goal-20261114-01/CONTRACTS.md`へ保存した。重複を含む22入口の報告には共有tailがあるため、異なる命令位置は1,031個。元入力hash・既存discoveryを照合し、private guestは変更・再生成していない。[採否と次の設計](REVERSE_ENGINEERING_PLAN.md)へ要点を整理した。

**採用：DXVK state cacheのwakeup indexを完全なshader組合せごとに一つへ。** 従来は同じ組合せの各stateに対してindexを追加し、shader登録で各indexからjobを作っていた。各jobが同じ組合せの全stateを走査するため、N stateでN job×N stateになる。既存pipeline cacheがあるので、実Metal compileもN²回だとは扱わない。

新adapterは全state／format／entry IDを維持し、既存`m_entryMap`で初出の完全なshader keyかを判定してindexを一度だけ追加する。読込みはworker開始前に行われ、追加の描画中lock・cache形式変更・worker増加・描画省略はない。reader／writer、registerShader／compilePipelinesはそのまま。初回compile、繰返しregisterShader、まだ揃っていないshaderの遅延は残る。

| 自作fixture：6組合せ×16 state | 旧 | 新 |
| --- | ---: | ---: |
| 保持し処理する異なるstate | 96 | 96 |
| shader→組合せのwakeup link | 208 | 13 |
| shaderが揃った後のjob | 96 | 6 |
| worker内のstate訪問 | 1,536 | 96 |

`tests/test_state_cache_work.py`は固定sourceの実読込index部分、registerShader、compilePipelines、key/hash/比較を使う。cacheレコード供給とGPU compilerは自作境界。1／2／16／128 state、前後順、共通shader、全graphics stage、compute、null／部分登録、並行登録を比較し、同じ必要stateと失敗対象を保持する。通常とASan/UBSanで成功。失敗対象の比較は自作の恒久失敗条件であり、実driverのOOMやMetal結果を検証したものではない。既存のpipeline失敗cacheと短いlookup lockは変更していない。

実ヘッダー付き`dxvk_state_cache.cpp`のWSL syntax検査もexit0（`local/stutter-goal-20261114-01/dxvk-final/`）。Mac一括経路へ同fixtureを追加した。通常fixtureの初回はvendorヘッダーのunused parameter警告を-Werrorにしたため失敗し、その警告だけを許容して実コードを変更せず検査を通した。既存CMakeのcache namespaceはprepare_dxvk.py自体をhashするため今回の変更も世代に反映される。

実データでは過去ログに751 valid state cache entriesがあるが、完全な組合せの分布は不明。次回は起動時に一行だけ`IOS_STATE_CACHE entries / shader_tuples / wakeup_links`を記録する。これをjob数・GPU cache hit数・時間短縮率とは扱わない。上表の訪問数93.75%削減も、その自作条件の値であってiPhoneFPS改善率ではない。

**設計まで：** textureは選択mipのpayload再コピーを省く論理viewを候補にしたが、元bufferの寿命、header／offset、allocatorと公開callbackまで条件化し未導入。完了待ちは、別threadの最大10,000回yield→1ms waitという元経路を確認した。通知待ちへの限定置換とReadFileExの本来の非同期化は別案にし、全state writer／世代／APC／取消／thread終了を必要条件として残した。実負荷のない推測で本番へ混ぜない。

今回のローカル区切りで、3系統の根拠・採否・測定／比較／棄却条件が揃った。アプリ／private guest／Apple／cloud build、push、署名、端末更新は行っていない。実機のFPS・瞬間停止・温度への効果は新GO後にまとめて確認する。

## 2026-11-14 午後：point拡大縮小の同じ座標を少ない除算で得る

`d3dx_surface.cpp`のCPU point samplingで、`floor(x*sw/w)`を商と余りの加算で進める経路を追加した。各pixelの横／縦の整数除算を、呼出し冒頭の商・余りと1行1回の縦計算へ減らす。中間RGBA snapshot、色key、色変換／pack、pitch／padding、LockRectのfallback・UpdateSurface・失敗順序は元のまま。等倍の既存fast path、同形式copy、bilinear処理も保持。出力は16384以下の範囲だけを追加経路へ送り、それ以外は元処理を使う。これは算術の上限を証明する条件で、実測に合わせた閾値ではない。新たな配列確保、精度変更、画質設定変更はない。

実関数の旧新比較は10,910ケース。15形式、filter、色key、失敗、cropに加え、奇数比、幅／高さ1、16384境界と16385のfallback、入力と出力のaliasを確認した。通常／ASan/UBSanで出力byte・padding・戻り値・API順序が一致。実ヘッダーを使ったtranslation unitのsyntax検査もexit0。公開stageの差分は`d3dx_surface.cpp`だけで、比較した関数本体と最終adapterの出力も一致した。

`local/surface-scale-20261114/result.json`のLinux x86_64／CPU1固定／7交互ペア（各100回）の中央値は下記。縦寸法はsource幅−7／destination幅−5なので、完全な正方形の値ではない。CPUで画像変換が必要な経路の自作負荷であり、GPU rendering、ゲーム内の利用頻度、iPhone FPS／温度の証明ではない。

| source幅→destination幅 | 旧→新の呼出時間 | 短縮 |
|---|---:|---:|
|64→127|147.70→67.26µs|54.5%|
|127→64|75.69→48.74µs|35.6%|
|320→640|4.920→1.974ms|59.9%|
|640→320|2.752→2.013ms|26.8%|
|1024→512|6.579→4.926ms|25.1%|
|512→1023|12.204→5.409ms|55.7%|

fixtureは既存Mac検査のsurface項目にも接続される。実Apple compilerと端末の結果は次のGO後。新しいapp／private guest／クラウドbuild、push、署名、インストールは実施していない。

## 2026-11-14 午後：追加候補の棄却とcache世代の補完

既存private ARM objectの4単位をPC内で読むと、算術helperへの静的call siteが計619箇所残っていた。これは実行回数・hotnessではない。公開headerのadd/sub helperだけをalways_inlineにする自作候補を、現ARM64/iOS/O1/strict条件で比較した。3形状＋128個の小さな呼出元を含む出力は旧新とも4,332命令／stackアクセス16／helper呼出0で同一。実ゲームの文脈との差は残るが、今回のfixtureには改善がなく、productionへ追加せずprivate guestも再生成していない。自作入力とARM結果は`local/cpu-helpers-20261114/`、既存objectの限定集計はPC内`private/analysis/cpu-arm-20261114-01/`。

BC2/BC3のCPU alpha展開は、packed色paletteと直接alpha取得、およびalpha配列を維持する別案を比較した。5形式・部分block・paddingを含む1,003,520ケースの出力は一致したが、Linux x86_64の固定CPU／7交互ペアでは遅くなった。直接alpha案はDXT3が101.63→159.41ns/block、DXT5が100.45→165.30、配列維持案は92.07→160.88、100.15→162.63。両方棄却し、既存BC1 fast pathとDXT2〜5の元処理を維持。`local/bc-alpha-20261114/benchmark*.json`に比較を保存。これはCPU fallbackの自作負荷であり、GPU圧縮textureやiPhone FPSの計測ではない。

追加したMoltenVK patch2本とmetrics headerを、永続pipeline cacheのsource namespace入力へ直接追加した。直前のbuild script変更でも今回のnamespaceは変わるが、今後patchだけを変えた場合も確実に更新するため。実CMakeのnamespace生成部分を新規の公開入力コピーで実行し、3入力を各1byte変更するとnamespaceが変わり、復元すると元へ戻ることを確認。最初のWSL実行はcmake未導入で実行不能だったため、既存Windows CMakeで確認した。依存／app buildは実施していない。

## 2026-11-14 14時前後：Metal待ちの分類と派生libraryの所有権

本人の「20時頃までできることをたくさん」に従い、既存の外部レビューを公開固定sourceへ照合しながら継続。新GOがないためアプリ／private guest／Apple build、push、署名、端末操作は実施していない。

### Metal compilerを通る待ちの観測

`patch_mvk_compile_timing.py`で固定`MVKSync.mm`の元処理を保ったまま、MSL library生成／function特殊化／最終pipeline生成／その他の呼出側待ちを分類する。GCDへ投入してからblockが始まるまでと、block実行中のhost時間も別に観測。既存ID 0〜20を保持して21〜26を追加し、5秒histogramと固定16件のIOS_STALLを共用する。毎frameのファイル出力、可変長sample、shader本文・識別名の記録は増やさない。

この分類だけではdevice monitor待ちとdriver内compile時間は分離できず、renderとcomputeの最終pipelineも同じ分類。dispatch blockの時間は同期API呼出しとその内部待ちであり、非同期compile完了時間やGPU時間ではない。要求IDによる対応付けもまだない。処理の重なりは因果証明にせず、固定sampleの欠落数も維持。新分類はNSErrorの警告／失敗を推測せず`failures_known=false`とする。

元のQoS・timeout・condition variable・error・completion・destroyは変更なし。遅いcallbackでcompilerが破棄された後、新しい観測が`this`へ触らないよう、dispatch blockへ渡す新情報は数値だけにした。元関数へ完全復元するbyte検査、元／新の実関数を自作dispatch境界で実行した4分類×成功／error／timeout後完了の12ケースを通常とASan/UBSanで通過。既存metrics/stall 5件、更新した解析器6件も成功。実AppleのObjective-C++ compile、実GCD／Metal実行は未確認。

### 派生libraryのcopy／並行mapを修正

`patch_mvk_library.py`が既存の圧縮引数copy削減と所有権修正を合成する。copy時はretainした元Metal libraryと値データだけを共有／複製し、所有raw pointerの派生mapをコピーしない。mergeによるowner付け替え後の派生libraryは新ownerで必要時に生成する。assignmentは一時copyを準備して旧派生mapの破棄を一度にし、自己代入とmacro可否も扱う。copyではC++データ確保が終わるまでMetal objectをretainしない。

派生mapの検索／登録だけをmutexで保護し、Metal compile・device monitor・負けた候補の破棄はlock外に置いた。1libraryのlockを長いcompile全体に保持する初期案は、既知variantの取得や別keyの並列生成を止めるため採らない。同時cold missが同じkeyを重複compileする可能性は残すが、登録は1個だけ、負けた候補はunique_ptrが解放する。これは同一key compile共有を実装したものではない。失敗variantのcacheとowner errorの元方針も変更しない。assignment／破棄／metadata変更とAPI利用を同時に行う契約は追加しない。

実copy／assignment／destructor／macro lookup、実codec／metadataを自作Metal境界で実行。copy元破棄後の寿命、新owner、旧派生データ破棄、自己代入、key順序・非macro項目・size、失敗cache、8件の同時cold生成後の単一登録、warm hitと別keyが停止しないこと、8thread×1000回の既知key取得を通常／ASan/UBSanで通過。元の圧縮copy検査2件も保持（payload 3→1、metadata 2→1）。追加のpatch検査は未知変更拒否と旧patchからの合成を確認。ASan/UBSanをdata-race検出やApple実行の証明とはしない。

この所有権問題の元コードでの不正参照は既に自作反例で確認済み。ただし現DXVK／hostにmergeの実呼出しは見つかっておらず、今回の端末停止の主因とはしない。実機FPS改善や重複compile削減率は未測定。次回GO後のMac検査と同じ公開build経路へ両変更を接続し、依存cache keyにも新patchと計測headerを含めた。


## 2026-11-14：2回目レビューの照合と次の全体分析

Fable 5.2／高の2回目回答を完了表示まで読んだ。初回のPresentとgap同一視、原因を限定する推測、system cache削除、compile待ち上限変更は撤回された。Rootのsource確認では、scope外のacquireを含め全commit hookが内部でgate入場するため、回答R1の「未対策のすり抜け」は現候補にはそのまま当てはまらない。R4のslot lockを保持したscheduled待ちや、commit後のsubmission参照も現実装では避けている。不要な二重対策は追加しない。

一方、R2/R3のmain-thread待ちとrun-loop再入、R6の実Metal scheduling、R7のscope内drawable待ちは残る確認項目。guest mainがUIKit mainでもあることは`ios_runtime.c`に明記されている。既存event pumpには再入guardがあるが、DXVKの全同期呼出しや通知の推移的な依存を覆う証明ではない。現candidateは実機導入可能と認定せず、Mac fixtureとlifecycle経路の確認を先に行う。意図的にbackground GPU送信を失敗させる追加の実機実験は、現appへ自動で混ぜない。

macro variantの浅いcopy／assignmentはRootの自作fixtureで再現済み。ただし固定DXVKと現hostの検索で`vkMergePipelineCaches`はAPI宣言・loader entryのみで、実呼出しは見つからなかった。これは網羅的な実行時不使用の証明ではないが、現在のカクつき対策と同じ優先度にはしない。macro利用の到達性と比率を確認し、function cache追加は別変更にする。失敗libraryをmapへ入れない提案も、ownerのerror保持と再試行契約を追う前には採用しない。operator=の削除案も現在の利用箇所と実Apple compile確認を前提とする。

本人の追加指示に基づき、次の相談はエンジン全体の分析へ広げる。公開の静的再コンパイラー／Win32 shim／D3D9→DXVK→MoltenVK→Metalと自作hostの説明、匿名集計だけを使う。CPU生成コード、flags／memory ordering、threadと同期、描画・shader、streaming、allocation、audioを比較し、部分逆コンパイルの候補を最大3系統へ絞る。実装やbuildをClaudeへ依頼せず、source根拠・必要計測・期待される処理量削減・互換性条件・棄却条件を求める。既存の採用案／棄却案を再提案だけで繰り返さない。

## 2026-11-14：ClaudeレビューとCPU依存待ちの訂正

PC版ClaudeのFable 5.2／高で初回回答を取得し、固定MoltenVK `458870543b9bcf6b0edd6f90aaa776707b310d96`の公開sourceで照合した。初回回答のDXVK参照版が本プロジェクトの固定snapshotと異なったため、`meizfl/skydream`の`ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/third_party/dxvk`を指定した追加レビューを送信済み。私有code／shader／生ログ／framework／資格情報は外部へ送信していない。

### 確認できたことと採用しない推測

- Vulkan pipeline cacheにあるのは変換設定／結果と圧縮MSLで、最終Metal PSOではない。cache復元にもMSL展開／library生成があり、取得後もfunction取得／定数特殊化／PSO生成が残る。保存成功をcompile消失やcache命中とみなさない。
- `getMTLFunction`、sampler生成などにはdeviceの`@synchronized`が残る。既存のcache mapのlock分離が全待ちを消すとはいえず、global lock削除やworker増加は採用しない。
- macro variant mapはdevice monitor外でlookup／追加される。copy constructorが所有raw pointerのmapを浅くcopyする一方、両destructorが各childをdeleteする。さらにassignmentは派生mapを更新せず、mergeはtop ownerのみ変更する。Rootの公開source抽出fixtureで、元のcopy／assignment／destructor本体を使い、Metal retain／releaseだけを自作handleへ置換して確認した。copy先が同じchildを所有し、assignment後は旧childが残ることを確認し、旧順序の2回破棄ではASanがheap-use-after-freeを報告した（意図した失敗、exit1）。`local/lifecycle-20261114/review_variant_ownership.py`に再現手順を保持。元の固定sourceにある欠陥で、今回のlock分離が導入した欠陥とは断定しない。実Metal／ゲーム内の発生・頻度は未確認。function再利用を追加する前に、library単位の同期、copy先は派生variantを持たず再生成、assignment／self-assignment／owner／失敗／lock順序を詰める。
- Present最大約2.125秒とframe gap最大約2.158秒の時刻対応はないため、同一停止や原因とする説明は採らない。各処理の最大値を足したり、未計測経路を除外したりしない。cache hit率／p99等の提案値も達成値ではない。
- binary archiveやMTLFunction再利用は候補。先に実際のMSL compile／function特殊化／PSO／drawable待ちを区別し、再利用される同一要求を測る。Apple管理のsystem cache削除、描画省略、精度低下、compile timeoutの無制限化は行わない。

### 休止gateで再現した循環と修正

実C gateを用いた自作反例で、consumerが入場後にCPU semaphoreを待ち、pauseがgateを閉じてconsumerを待ち、そのsignal producerが閉じたgateで待つ循環を確認した。ローカルの`review_dependency_cycle.c`は旧順序の循環を観測し、検査終了のためだけに外部signalで回収した。本番で偽のsignalを出す対処はしていない。これは未導入候補の修正であり、iPhoneの観測済み停止原因という意味ではない。

MoltenVKのcommand／presentとも、CPU側の`encodeWait(nil)`をscope入場とearly enqueueより前へ移す。commandは`_emulatedWaitDone`で一度だけ消費し、presentは元のinline nil waitを除去する。native eventのGPU encodeWaitとcompletion signal位置は保持。意味が依存する`MVKSync.mm`の固定hashも確認する。新しい待機上限や成功偽装、workの破棄は追加しない。

対象3検査はLinuxで通常／ASan/UBSanとも2成功＋実Metal1明示skip。実C gateはCPU依存が未解決のままpauseが返り、resume後にproducer／consumerが一度ずつcommitし、参照数が戻ることを確認。元byteへの逆変換で7commit／2scope以外の既存処理も保持を検査した。ASan/UBSanはdata raceの証明ではない。

次回GO後のMac fixtureにはMTLEvent／MTLSharedEventの別queueによる将来signal、64slot満杯と先行enqueueを追加した。event待ちbufferがproducer未commitでもscheduledになるかを実driverで検査し、止まれば外部deadlineで失敗させる。fixtureのqueueは66buffer容量を明示し、既定queue容量で検査自身が停止するのを防ぐ。Apple compile／実行、UIKit main callback依存、watchdog、iPhone復帰は未確認。追加source確認では`MVKEventEmulated::encodeWait`のCPU待ちもcommand内にあり、submission冒頭のsemaphore事前待ちだけでは全依存を覆わない。native eventを選ぶ実端末経路とemulated経路を区別し、whole-execute scopeの安全性を一般化しない。保留中の2回目レビューで反例が出れば取り込む。

アプリ／private guest／クラウドbuild、push、署名、端末更新は未実施。新候補のFPS／発熱改善はまだ主張しない。

---

## 2026-11-14 09時台：実機ログを受けた待機分離と休止処理

本人の「全て取り掛かって」「再開して」に基づき継続。新しいGOはまだないため、アプリ／private guest／クラウドのbuild、push、署名、端末更新は未実施。USBの再接続も依頼しない。先のdescriptor容量切替、MoltenVK cache比較・hot lookup・copy削減候補は維持し、実機未確認の効果として扱う。

### 表示が止まる場所を5段階に分ける

既存の`present_host`最大約2.125秒だけでは原因を特定できないため、固定DXVKの実API呼出しへ`image_acquire`（通常と先行取得の両方）、`queue_present`、`queue_submit`、`fence_wait`（timeout再試行を含む全区間）、`submission_wait`（submit threadの結果公開待ち）を追加した。既存kind 0〜15を動かさず16〜20を追記し、解析器も同じ対応へ更新。5秒histogramと固定16件の停止サンプルを再利用し、毎frameのlogや動的確保を追加しない。重なる区間を足してGPU時間や原因時間と解釈しない。

元のVSync／FPS cap／semaphore／fence／wait timeout／戻り値／エラー分岐は維持。計測部分だけを除くと元ファイルへ完全に戻ることと、実Present/acquire関数の自作driverで成功・SUBOPTIMAL・失敗・timeout・先行取得・frame index・limiter呼出しを確認した。実DXVK 3 translation unitのsyntax検査も成功。これは停止を解消した証拠ではなく、次回の判断に必要な区別である。

### バックグラウンドでのGPU送信拒否を防ぐ候補

実ログ末尾の`BackgroundExecutionNotPermitted`に対応し、[Appleの休止要件](https://developer.apple.com/documentation/metal/preparing-your-metal-app-to-run-in-the-background)に合わせたアプリ内gateを追加。WillResignActiveで新しいnative submissionの受理を止め、すでに受理したencode→enqueue→commitが終わるのを待ち、そのcommand bufferが**scheduled**になったことを確認して戻る。DidBecomeActiveで待機を解除。UIKitのmain threadはguest処理を進めずrun loopを処理し、workerはcondition variableで待つ。休止のための新しい空command buffer送信や`waitIdle`、GPU完了待ちは追加しない。

固定MoltenVK全sourceを確認し、実command bufferのcommit 7箇所をstrong host hookへ接続。residency setの別のcommit 2箇所は同じGPU送信ではないため、header hashを固定して区別する。commit直前だけを止める初期案では、先にenqueueされたbufferを取り残す可能性があった。最終候補は通常submissionとpresentの`execute()`全体へRAII scopeを置き、受理済みscope内のcommitを先に終えられるようにした。元のcompletion callback、queue順序、error、resource releaseは削除しない。未送信処理を成功扱いしたり破棄したりしない。

gateは未scheduledのbufferを最大64件保持する固定領域で、通常時はGPU待ちを追加しない。満杯の場合だけschedulingを待ち、nested/early-enqueue時には先に現在のcommitを行ってからscheduledを確認する。scheduledになった全entryを次の送信で回収し、過去のburstの上位slotへ資源を残し続けない。pause時は全entryを解放。保持するのはMetal command bufferの参照で、CPU ABIやゲームのメモリ形式を変えない。

実C gate＋自作driverで、休止中の保留、進行中commit、enqueue後の休止、2回pause、main loop復帰、満杯、再入、burst後の参照回収、4thread×2,000回と100回のpause/resumeを検査。通常とASan/UBSanで成功。計測・解析・Presentを含め対象15検査成功、実Metalの1検査はLinuxでは明示skip。実Metalの2queue／byte一致／scheduled確認を行うMac fixtureは用意したがまだ実行していない。UIKit通知、実MoltenVK並行動作、iPhoneのホーム移動／復帰、watchdog deadlineは未確認であり、ローカル結果だけで修正成功とはしない。

Linux x86_64・CPU1固定・単一送信thread・自作の即scheduled driver・100万回×7交替ペアでは、直接呼出しの中央値1.764nsに対し、scope＋gate経由129.546ns。約128nsの追加CPU処理で、軽量化やiPhone overheadの測定ではない。最大64slotの保持／走査とdriverの実status照会は実機で評価する。原数値・対象syntax／ASan出力は`local/lifecycle-20261114/`。

### 次回Macへの統合と残る範囲

固定MoltenVKの元Package(iOS only) schemeは未使用のdynamic frameworkもlinkするため、アプリ内strong hookを解決できない。必要な`MoltenVK-iOS-static` targetだけをbuildする経路へ変更し、同じpublic headersを収録する。archiveで3つのstrong参照を確認し、最終appで`gpu_gate_metal.m`へ解決する。弱いfallbackや未解決symbolの無視は使わない。変更したdependency keyとcache namespaceで旧archiveを混ぜず、Mac gateは実際にpatchしたsource bytesと全commitの列挙も確認する。これは手順準備まででApple compile/linkは未実施。

起動logが常にbuild3と表示していた箇所は、実bundleのCFBundleVersionを読むよう訂正した。bundle version自体やprivate guestは変更しない。

単独の初回MSL／PSO生成、driver内の待ち、実機FPS・発熱・水中表示は残る。go後の一括buildでApple compileと公開Metal fixtureを通し、本人が戻った後に同じsave／画質で静止→カメラ→魔法／攻撃、同じ動作の反復、ホーム→復帰、cache保存後の再起動を比較する。今ある数値だけで描画省略・待機削除・ゲーム関数置換は行わない。

---


## 2026-11-14 09時：build5の実機ログ回収（途中まで＋別途末尾）

本人の保存・終了・ロック解除・再接続後、既存USB信頼とApp IDを照合してbuild5から読み取りだけを行った。ログ先頭の固定文言にはbuild3と残っているが、InstallationProxyの実BundleVersionは5。全量4,439,847,459 bytes、全18範囲の約22MiB弱を先に回収し、サイズ・mtime不変を確認した。その後の全体走査は本人の外出／USB切断に合わせて停止。少なくとも2,147,483,648 bytesまで読み、重複する2種類の警告以外を保持した。全量回収・全量hash照合の完了とは扱わない。元ログ・ゲーム・セーブに変更なし。

- sparse sampleの完全行129,914件のうち、descriptor pool超過警告129,370件（dynamic UBO 79,604、combined sampler 49,766）。sampleの完全行bytesの99.89％がこの警告だった。全ログで同数・同比率と断定しない。元のline-buffered stderrへ毎回書く経路で、個別確保に加えログI/Oも負担候補。上のpool容量事前検査はこの実機症状に対応するが、新版実機で解消した証明はまだない。
- 連続して回収できた先頭部分の約1,043秒、IOS_TIMING 1,513記録、32,269 frame間隔を解析。invalid／duplicate記録0。50ms超181回、100ms超122回、250ms超60回、最大2,157.534ms。ロード／menu／設定操作も含み、全件を実プレイの停止とは扱わない。
- graphics pipeline作成1,867件、100ms超220件、最大571.642ms。生成lock待ち385件、最大368.061ms。shader module最大0.920ms、D3D shader作成最大0.991ms。別々のthread／nested区間を合算せず、pipeline作成・共有待ちを優先調査する根拠とする。
- Present host最大2,124.922msの別経路もある。旧build5には新しい個別slow-span記録がなく、5秒windowでの同時発生だけでは正確な因果／どの操作かを確定できない。単に全停止をshader compileに帰属させない。
- 60上限で取得できた20windowのFPS中央値46.255、最大57.47。上限なし7windowの最大59.20。大部分は30上限（178window、中央値29.90）で、設定変更をまたぐwindowもある。全体平均を60上限の性能として扱わない。
- 回収範囲の205端末状態はすべてthermal=0、memory_warning=0。headroom最小約4,660MiB。iOSの熱圧迫・メモリ警告はこの範囲では観測していないが、表面が熱くない／未回収区間も同じ、という証明にはしない。
- 別途回収した末尾で、pipeline cache checkpointは12,269,323 bytes／stored=1／187.225ms。実機の保存処理は成功したが、初回起動のoffered_bytes=0で、次回再利用／命中はまだ不明。その後のVK_ERROR_DEVICE_LOSTはMetalがbackground GPU送信を拒否した明示理由付き。プレイ中のメモリ不足と混同しない。

次の優先順位は①pool超過による個別確保と大量ログを修正候補で止める、②初回pipeline生成／共有cache待ちとPresent停止を個別時刻で分ける、③background移行時の新規submit停止・実行中queue退役・復帰の状態遷移を設計する。③は単に待ちを削る、main threadから盲目的にwaitIdleする、GPU失敗を成功扱いする変更にはしない。現候補のapp／クラウドbuild・push・署名・更新は未実施でgo待ち。

生ログ・範囲情報・数値集計はRootの`private/iphone-build5-20261114-0856/`に保持。`collection-scope.json`が中断範囲、`sampled-latest/ranges.json`が疎な範囲とhash、`full-scan/timing-analysis.json`／`prefix-summary.json`／`slow-windows.json`が先頭部分の解析。全量を保存したフォルダー名とは扱わない。

## 2026-11-14早朝：build5試遊後の改善（次のgo待ち）

本人の試遊報告は60 FPSに達した場面と発熱の軽減。一方で負荷／視点変更時の瞬間停止、操作のやりにくさ、水中の茶色表示が残った。新候補の実機計測はない。元ゲーム・セーブ・旧IPA・private guestを保持し、アプリ／クラウドbuild、push、署名、端末更新は行っていない。以下のコンパイルは自作のhost検査用と、RootのPC内だけのshader解析用。

### 操作と表示言語

`mobile_ui.m`の各操作ボタンをSF Symbols＋A/B/LT/RT等の小さな識別表示へ変更。移動スティック、全gamepadボタン、配置保存、同時タッチと物理パッドの経路を保持する。設定・保存・キーボードは44ptの記号ボタン。SF Symbolが得られない通常操作ボタンには元の識別表示を残す。設定・通知・熱状態はEnglishを既定とし、Languageで日本語を選択／保存できる。既存の日本語専用版からの更新も未選択ならEnglish。ゲーム本文・音声・画質の数値には言語設定を適用しない。UIKitの実コンパイルとレイアウト目視は次のgo後で未実施。

視点は以前の80ms一定保持から、受信周期8〜34ms＋8msの余裕を保持した後に1周期で減衰する方式へ変更。120Hz入力では約24.7ms、60Hzでは約41.3ms、30Hzでは約74.7msで停止する。連続入力の速度は維持し、ゲームのdeadzone補正より上の速度成分を減衰させる。指を離す／cancel／clearは即時解除。重複・逆行timestampは新しい速度として採用せず、止まった後の過去サンプルを再生しない。実Cの周期・境界・方向・微小値の非増幅・同時読み書き検査を通常／ASan/UBSanで通過。実機の感触は未確認。

### Descriptorの個別確保を避ける

所有build2ログでは、dynamic uniform bufferの6144枠超過警告24,925回、combined image samplerの4096枠超過警告16,031回を一実行で確認した。別実行でも14,359／16,111回。build5のログではなく、今回試遊時の頻度は未知。固定MoltenVKはpoolが足りなくても個別descriptorを動的確保してVK_SUCCESSを返すことがあり、失敗時だけ新poolへ進む元DXVKでは小さな確保と警告が続く。

`prepare_dxvk.py`で実layoutのstatic/dynamic変換後に2種類の必要数を一度だけ数え、poolの元の容量に入らない要求を元のrollover経路へ渡す。per-drawのmap／走査／mutexを追加しない。成功時だけ使用数を進め、reset成功時だけ戻す。固定meta layoutは0 dynamic UBO／最大2 combined samplerとして保守的に数え、全6meta実装と変更6ファイルのpinを確認する。元のqueue所有、GPU完了後の再利用、1回の再試行を保持。

実公開pool/layout/contextコードと自作MoltenVK相当driverをつないだ5frame×8192要求では、旧版はwhole pool 5個＋dynamic UBOの個別確保73,848個＋combined sampler 122,855個。新版はwhole pool 12個、両種の個別確保0個。旧新の保持値／順序digestは`6669998874461262777`で一致し、warm後の追加pool生成0。正確なfit、失敗／reset失敗、並行する別context、退役前のデータ保持も通常／ASan/UBSanで確認。変更された実4translation unitのsyntax検査も通過。

whole poolが5→12個へ増えるため、メモリ消費が減る変更とは主張しない。数値は自作負荷の対象2種類だけで、実iPhoneのallocator時間／FPS／発熱は未測定。容量を増やす、描画を間引く、まだGPUが使うdescriptorを先に再利用する変更ではない。

### 水中描画と浮動小数点の互換性

[Skyrim Android 0.30.15](https://github.com/ttom795/Skyrim_Android/releases/tag/0.30.15)の水表示とfloat emulationの記述を手掛かりに、固定DXSOの実演算を確認した。既定の`FloatEmulationByDefault`だけをtrueへ変更し、明示`d3d9.floatEmulation`設定の優先とコンパイラー本体を維持する。RCP／RSQ／NRMのNMin、LOGのNMax、strict POWは既存実装。水・霧・反射の無効化や精度引下げではない。追加ALUの負担はあり得るので速度向上策とは扱わない。

Microsoftの[NRM](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/nrm---ps)と[LOG](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/log---ps)はゼロ境界に有限の最大値を定める。一方で単独RCP／RSQのゼロはinfinityという仕様もあり、このmode全体をD3D9全命令の厳密な再現とは呼ばない。

公開DXSOを実際に通した自作PS3の5演算×2入力×off/on＝20件を検査。Linux software Vulkanの1pixel RGBA32float描画では、普通の入力の出力はすべて旧新一致。LOG(0)は-inf→-FLT_MAX、RCP／RSQ(0)はinf→FLT_MAX。NRM(0)は両方0、POW(0,0)は両方1で、このbackendではそれらの旧NaNを観測したとは言わない。公開の補正版SPIRV-Crossでonの10件をMSLへ変換した。次の一括Apple検査には既存alias3件に実行時入力の自作5件のiOS Metal compileと、下記のMac数値readbackを加えた。まだAppleで実行していない。

Rootが所有BSAを読み取り専用で解析し、水を指す定数を持つ48shaderをPC内で抽出・変換した。48件合計でRCP205／RSQ91／NRM138／POW67命令。48/48でDXSO→SPIR-V→MSL変換成功。水中passが実際にどれを使ったか／正しいpixelsになったか／Apple Metalがどう実行するかは未確認。私有bytecode・MSL・ログは`private/analysis/water-20261114/`にのみ保存し、公開fixtureには含めない。元BSAは変更しない。cache namespaceはadapter hashを含むため旧Vulkan cacheと区別される。

**Metal算術の未検証点。** 固定[SPIRVToMSLConverter](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp)はshaderのfast-math flagsを渡し、[MVKDevice](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm)がMetalのmath mode／FP関数を選ぶ。今回の定数入力10件と実行時入力5件では実Cross出力のflagsがすべて`4294967295`だった。MSLのNMin/NMaxはprecise::min/maxになるが、それだけで前後の演算まで全hardwareで期待どおりと判断しない。全shaderのfast mathを無条件に止める変更はしていない。[AppleのmathMode](https://developer.apple.com/documentation/metal/mtlcompileoptions/mathmode?language=objc)も確認した。

`tests/dxso-float/metal_check.mm`を準備し、次の許可済みMacビルドで、自作5fragmentへzero／normalの入力を描画直前にconstant bufferで渡し、fast／safe両方で計20件を1pixel RGBA32Floatへ実描画しreadbackする。Metalコンパイル時には入力値を渡さず、定数畳み込みだけの検証にしない。alpha-testは既存function constantでAlwaysへ指定し、未描画のclear値が期待値に一致しないようにした。Python側でも20件・両mode・全laneのbitから独立に期待値を検査する。NaN／Inf／欠落／重複／未実行を成功にしない。外部deadlineを設け、失敗時はlogと数値を保持してhost配布を止める。公開の実DXSO→Crossまでと数値reportの拒否検査はローカルで確認したが、この新Objective-C++検査自身のApple compile／GPU実行は未実施。Macで成功しても実iPhoneの水中描画の合格とは別。

**定数畳み込みを除いた追加再現。** DXSO fixtureへDEFを持たずc0を読む5演算×off/onの10programを追加し、変換検査は計30件。Linux software Vulkanで実行時constant bufferを変えて20drawを行った。normal入力は全演算で旧新同値。NRM(0-vector)は旧方式のXYZがNaN→互換方式のXYZWが0、POW(0,0)は0→1、RCP／RSQはinf→FLT_MAX、LOGは-inf→-FLT_MAX。独立したbit／数値期待値で照合した。これは前段のDEF定数shaderでは見えなかった差であり、Metalコンパイラーでも同じ試験が必要な根拠。出力・再現sourceは`local/overnight-20261114/runtime-float-check/`。実際の水中passでゼロ入力が発生した証拠や、iPhone描画修正の確認ではない。

### 停止区間の低負荷診断

既存の5秒histogramに加え、50ms以上の処理の開始／終了を記録する`IOS_STALL schema=1`を追加。各report間にframe gap4件＋その他12件を保持し、満杯／競合で落ちた件数も出す。不完全なサンプルであり、全停止の完全traceではない。書込み側はatomic flagを1回だけ試し、待機・allocation・ファイルI/Oをしない。log整形はlock外で5秒ごとの既存reportにまとめる。anonymous thread ordinalはprocess内だけで、OS ID・address・ゲームの名前やbyteを出さない。

`fields=kind,thread,start_ns,end_ns,failed,estimated_end`。kindは`SkyrimPerfKind`の既存16分類、thread=0は取得不可。`end()`とPresent/frame/pacerは既存の測定端点を使用し、elapsedだけの`record()`は推定端点として1を付ける。UIKitの休止／復帰境界をまたぐspanは除外する。histogramの既存意味は維持。重なるcompiler／wait／frame区間を合計して原因時間にしない。VSync待ちや同時実行も含むhost wall timeで、GPU時間や因果関係の証明ではない。

実Cで閾値直前／一致、満杯とframe枠予約、lock競合時の即時断念、drain、無効時刻、時計失敗、休止をまたぐ区間、ID上限、4thread×20,000記録のaccepted＋dropped一致を通常／ASan/UBSanで確認。既存histogram3件とread/audio透過性4件も通過。Linux x86_64、CPU1個固定、旧新交替7ペア×20万呼出しの中央値はfast record 16.04→17.14ns、開始時計を含むend 64.74→65.99ns。満杯時のslow recordは15.90→51.63ns（追加時計と固定sample処理）。これは自作CPU検査で、iPhone overheadやFPS改善の測定ではない。原数値は`local/overnight-20261114/metrics-benchmark/result.json`。

### 音声の不要なpolling起床を減らす

実`xaudio2.c`の通常再生では、約40ms以上の音声がqueueに残っている間も1msごとに確認していた。元のPCM、mix順、callback、queueの4pass分の予備、500ms停滞時の復旧経路は保持し、余剰queue時間の半分だけ休む（1〜5ms）方式へ変更。SDLが交渉したsample数／rateが不明・範囲外、またはdevice quantumが3passを超える時は元の1msを使用する。thread作成前に得る不変のdevice rateを使い、mutableなmixer rateを追加で読むことはない。

実engine loopと自作PCM sink／時計による118条件で比較。22.05／44.1／48／96kHz、256／512／1024 samples、位相・sleep超過・割込み、stop/resume、stall/recovery、deviceなし、2048samplesのfallbackを含む。通常条件でpacket順・digest・個数は一致し、新たなunderflowなし。代表約2.97秒／48kHzでは待機呼出し2,936→1,430回（約51％減）、queue取得3,536→2,030回（約43％減）。300packetのdigestは`3186091944889578302`で一致。通常・ASan/UBSanと変更された実xaudio2 unitのsyntax検査を通過。大きなdevice quantumは旧新完全一致。数値は自作条件で、実device音声・発熱・FPS改善を証明しない。停止への反応が最大約4ms余分に遅れる可能性はある。

[SDLのqueue量](https://wiki.libsdl.org/SDL2/SDL_GetQueuedAudioSize)はhardwareへ未送信のbyteであり、正確な再生位置とは扱わない。[Appleのtimer削減指針](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/MinimizeTimerUse.html)も参照した。処理完了のevent化ではなく、既存pollingの頻度を減らした候補。

### 相手のいないthread通知の走査を省く

公開`kernel32.c`の`wake_waiters`は、通知対象を誰も待っていなくても全待機threadと各wait setを走査する。各host waitableへ待機entry数を追加し、元の`g_wlock`内でcond wait直前／全return後に増減する。ゼロの場合だけ走査を省き、対象がある場合は元の通知順・全対象へのsignalをそのまま使う。重複handleはentryごとに数える。32bit飽和時は永久に元の走査へ戻し、wrapによる偽ゼロを作らない。元atomic／fence／lock／timeout／WaitAllの消費規則は不変。CPU ABIやguest memory layoutは変更せず、host内部objectに4byteのcountを足す。

実handles／wait／signalコードとpthreadを使用した自作検査で、event・mutex・semaphore・thread完了、WaitAny／All、即時成功、timeout、spurious return、重複handle、複数waiter、飽和と1,600往復を通常／ASan/UBSanで確認。変更された実kernel32 unitのsyntaxも通過。1,000通知では無関係な32thread×4handleの比較が128,000→0回、対象ありの比較128,000回と通知32,000回は旧新同じ。

Linux x86_64の自作比較（CPU1固定、7交替ペア×50万呼出し）の中央値は、無関係な8thread×1handleで23.94→7.13ns、32×4で112.00→7.50ns。待機threadゼロでは6.93→7.19nsと少し増える。登録／解除のcount処理だけを同じlockで比較すると1entryは7.92→8.88ns、64entryは7.33→108.88ns。ready状態から即時returnする経路には登録処理を足さない。原数値は`local/overnight-20261114/waiter-benchmark/result.json`。iPhoneでの通知頻度・節約時間・性能改善量は未確認。元のCloseHandleのwaitable寿命方針も変更しない。

### 次のログを読む準備

`scripts/analyze-frame-timing.py <1起動分のログ>`はread-onlyで数値JSONをstdoutへ出す。20binのp50／p95／p99を範囲として出し、50／100／250ms超の数と最大値、長いframe gap最大20件と同時刻の処理最大12件を表示。匿名threadの同一／別／不明、推定端点とsample欠落数を保持し、重なる時間を合算して原因時間にしない。ゲームのpath／shader名／raw行は出力しない。未知schema、壊れた数値、二重timing行は集計から除外して件数表示。逆行clockは複数起動混在の疑いとして拒否するが、単調なclockだけでは全起動境界を検出できないので必ず1起動1ファイルで使う。

自作境界・重複／不正行・読み取り上限・percentile範囲・重なりと実C recorderからの出力の6検査を通過。最新実機ログは未回収で、まだ実際の主因を選んだ結果ではない。

### 測定の訂正と組合せ確認

以前のBC1比較用fixtureで、benchmark分岐だけ出力bufferの未使用padding／tailを初期化せずdigestへ含めていた。通常の1,003,520ケース比較は毎回sentinelで初期化しており、この不備はない。製品decoderは変更せず、benchmarkの初期化を計時前へ追加した。旧benchmarkのdigestは出力一致の証拠に使わない。

修正後のCPU1固定・旧新交替7ペアでは、DXT1は58.665→38.604ns（約34％短縮）、DXT3は85.324→89.558ns（約5％増）、DXT5は93.662→92.307ns。各形式の旧新全runでdigestが同一になった。全形式の高速化とは主張せず、旧表のBC1時間値を現行の根拠として使う場合はこちらの測定へ置き換える。元データは保持し、新結果は`local/overnight-20261114/bc1-benchmark-corrected/result.json`。CPU展開の実機頻度は不明で、圧縮textureをGPUが直接sampleする通常経路のFPS向上ではない。

今回の最終runtime adapterで、waiter／APC／shader待合せ／directory／I/O・音声観測／入力・CPU ABI／histogram／停止span／解析器を組み合わせた27検査が成功。個別のASan/UBSanと実source syntax結果は上記を再利用した。BC1の通常比較と新Metal数値report検査も成功。Mac／iPhone未実行の項目をローカル検査で置き換えたとは扱わない。

### MoltenVKの誤ったcache一致と準備済みlibraryの待ちを修正

固定MoltenVK v1.4.0（4588705）の実`SPIRVToMSLConversionConfiguration::matches`に、dynamic buffer descriptorとdiscrete descriptor setを片方向にしか照合しない問題があった。[上流の修正cbe4979](https://github.com/KhronosGroup/MoltenVK/commit/cbe497918d1d24772de4753eaa199be74fa62d07)の9行だけを同じ形でbackportした。実DXVKにもUBO数でstatic/dynamicを切り替える経路があるが、今回の水中症状の原因と確定したわけではない。

完全な固定公開headerと実比較関数を使い、自作の3stage×descriptor集合×discrete setの12,288組合せを比較。旧版の不適合な一致3,340件→修正版0件。順序の違い、重複entry、別stageの無関係なdynamic descriptor、意図的に非対称な「未使用resourceは無視する」規則も維持した。通常／ASan/UBSanで通過。これは作成した設定集合であり、ゲーム中の誤描画回数ではない。

別に、[上流issue2863](https://github.com/KhronosGroup/MoltenVK/issues/2863)と同じ全体lockを固定版で確認した。`MVKPipelineCache::getShaderLibrary`ではSPIRV-Cross変換とMetal library compileの間も全cacheのlockを保持し、無関係な準備済みshaderの取得・保存用snapshotも待つ。今回の候補はcold compile専用mutexを1個追加し、同じcacheのcold compile同時実行を元と同じ1件に制限する。既存libraryの検索だけ短い元lockで行い、新libraryの公開と`_dataSize`無効化を同じlock内で完了させる。外部同期指定の経路、実shader変換、Metal compile、wire形式は変更しない。

自作の変換／Metal待ち境界と実cache関数を接続したCPU検査では、cold処理を止めたままのhot取得・snapshotが旧版では完了せず、新版では完了した。8件の同一要求による重複compileは0、別keyを含めcold同時実行上限1。変換失敗・compile-required・外部同期・公開時allocation失敗・compile中のmerge・候補の破棄と再試行も検査し、通常／ASan/UBSanで通過。snapshot検査は公開entry数と実`_dataSize`の無効化の検査であり、実Cereal byte列やMetal driverをCPU fixtureで再現したという意味ではない。元のfeedback分類も保持した。単独の初回compile、function specializationのdevice lock、driver内部待ちは残る。

追加確認ではfixtureのshader library格納先も固定版の実`MVKSmallVector`／allocatorへ置き換えた。513設定への拡張と4threadの計4,096既存設定lookupを重ね、格納領域の再確保後もlibraryの同一性と内容を保持。既存のfailure／cold上限検査を含め、Windows ClangとLinux GCC＋ASan/UBSanで成功。Foundationの未使用view型とMetal境界だけを自作宣言にし、実containerはbyte一致の公開headerから使う。

変更は元checkoutを保持する`MoltenVK-ios-cachefix` build stageへ適用し、全入力／出力hashを照合する。依存出力は新しい`local/ios-dependencies-cachefix`へ分離。build5の完全な旧configuration key・SDK等が一致する時だけSDL/FFmpeg/vkd3dを再利用し、MoltenVKは次回goで一度再compileする。cache namespaceに両patcherとtemplateを含め、古いserialized cacheを混ぜない。次回Mac gateで実際にpatchしたstageのbyte一致とCPU検査を再確認する。現時点でApple compile、実Metal並行実行、iPhone FPS／発熱改善は未確認。新規cloud実行・push・署名・private guest更新は行っていない。

### 圧縮シェーダーデータの余分な引数コピーを削減

固定MoltenVKの`MVKShaderLibrary`コンストラクターとcache復元の`addShaderLibrary`は、圧縮MSLコンテナーを`const`値引数で受けていた。復元時は呼出しの2段でvectorのdeep copyを作り、さらにlibrary内部へ保持用copyを作る。宣言・定義の計4箇所だけをconst参照引数へ変更し、library内部の代入は維持した。元のcodec、特殊化判定、Metal compile、所有するbyte、同期処理、serialized形式は不変。内部C++ symbolは変わるため、次回のMoltenVK全体の再buildに含め、旧archiveと部分的に混ぜない。

固定の実`MVKCompressor`クラスと実コンストラクター／cache挿入関数を使用し、codec・Metalの境界だけ自作にした検査を実施。空〜1MiB、特殊化引数の有無、呼出元の後続変更と一時オブジェクトの破棄を含む24条件で保持内容が一致。64KiB payloadのvector確保はcache復元3→1回、直接生成2→1回。通常／ASan/UBSan、最終patch stageの全byte照合と再適用を通過した。Apple compile・本物のcodec性能・Metal実行はこの検査に含まない。

CPU1個固定、GCC -O2、旧新交替7ペア、自作復元3000回の中央値は256Bで163→124ns、4KiBで583→389ns、64KiBで96.5→11.2µs、1MiBで2.77→1.15ms。これはallocatorと自作の非圧縮codec／compiler境界を含む局所条件で、サイズによるallocator挙動の差が大きい。実際の圧縮shaderの大きさ・復元頻度・iPhone FPSやカクつき改善率の証明ではない。原数値は`local/overnight-20261114/mvk-compressed-benchmark/result.json`。Macの既存gateへ同じ所有権検査を追加した。新しいbuildは実行していない。

**見送った別案。** 特殊化macroが空なら毎回の引数走査を省く条件分岐も比較した。自作64定数条件で空の場合はGCC約221→3nsだったが、macroが8個ある場合は約623→743nsに悪化。Windows Clangでも8個時は約1255→1333ns。作成時に判定する案にはcopy代入時の既存状態の扱いも関わるため、今回の小さな最適化へ混ぜなかった。Rootの水関連48shaderの静的変換ではmacro0・普通の特殊化定数6〜12を観測したが、全ゲームshaderや実pipeline設定の代表とは扱わない。候補はlocal実験内に残し、本番には適用していない。`tests/metal-alias/alias.cpp`の特殊化件数の表示だけを次の公開検査用に保持した。

追加のWindows Clang検査では、MSVC標準ライブラリが大きなvector確保にalignment用byteを足すため、payload byte数とallocation要求byte数を同一視した検査が失敗した。製品コードの内容・所有権の比較は通過しており、検査だけを実vectorの確保要求サイズで校正する方式へ修正した。Windows ClangとLinux GCC＋ASan/UBSanで旧3／2回、新1／1回を再確認。実クラスやコンストラクターを計数用に改変していない。失敗のstderrも検査出力へ残すようにした。

同じ固定sourceの`MVKMTLFunction`も、変換結果のstring／mapを値引数でcopyした後、memberへ再copyしていた。宣言・定義の2箇所をconst参照へ変え、member代入とMetal retainを維持した。圧縮payloadの4箇所と合わせ、helperが元hashから正確な6箇所だけを変更する。最終組合せstageは`local/overnight-20261114/mvk-cache-review/combined-patched-v2`。古いstageから製品を組み立てない。

実metadata型・実constructor（Objective-C retainの境界のみ自作）で、map 0／1／8／32件、短い／長い名前、全flag、workgroup、呼出元の変更・一時値の破棄を含む16条件を検査。memberの所有copyは保持し、引数copy1回だけがなくなる。標準ライブラリ固有のmap sentinel確保は実型で別計測し、Windows ClangとLinux GCC＋ASan/UBSanで一致した。実Metal objectのretain／releaseを実行した検査ではない。

実metadata型のcopy経路だけを自作holderで測ったGCC -O2、旧新交替7ペアでは、map 0／1／8／32件で中央値36.8→27.2ns、133→85.8ns、667→337ns、2798→1382ns。小さなCPU処理の削減であり、Metal compiler時間やゲーム全体の改善率ではない。原数値は`local/overnight-20261114/mvk-metadata-copy/result.json`。Windows Clangの補足比較は同directoryの`result-windows.json`。実機での呼出頻度・寄与は未測定。

### 初回コンパイルの残課題と第三者調査

[PPSSPPの公開計測](https://github.com/hrydgard/ppsspp/blob/master/docs/shader-compilation.md)では、M1／MoltenVK 1.4.2の特定のcold起動でMetalのMSLコンパイルが主な待ちだった。共有cacheを使わない比較もあるが、ゲーム・機器・cache保存方式が本件と違うため、その倍率をSkyrimへ適用しない。本件は保存cacheを維持し、準備済みlibraryだけ先に取得できる変更を採用した。単独の新規MSL compile、Metal function specializationのdevice lock、PSO作成は残る。

固定DXVKのstate cache workerは既にcondition variableで休み、shader登録が揃った既知pipelineを先行生成する。全shaderを毎frame作る方式でも、idle時に常時pollingする方式でもない。worker増加やcacheの無効化はコンパイルと熱・メモリの競合を変えるため、実機の停止区間とcold／warm比較を見てから選ぶ。未準備drawの省略、前のpipelineの流用、全shader一律の低精度化は描画を変えるので今回の候補に入れない。

[MoltenVK issue2872](https://github.com/KhronosGroup/MoltenVK/issues/2872)には、1.4.1以降の状態追跡変更でclear後にdescriptorを再bindしない回帰が報告されている。本件の固定1.4.0をその症状の原因とはせず、新しい版への一括更新も行わない。今回採用したcache比較の上流修正は独立した小変更として検査した。

次に切り分ける順は、①`IOS_STALL`と`graphics_pipeline`／`d3d_shader_wait`の重なり、②同じ魔法・視点操作の初回と反復、③checkpoint後の再起動。host pipeline時間はMSL compile・PSO・lockを含み得るため、これだけでMetal compilerが主因と断定しない。必要な場合だけMoltenVK内の段階計測を追加し、ゲーム関数の部分逆コンパイルはshader以外の上位経路が判明した時に広げる。

### 次回の実機確認

次のgoで一度だけ公開hostをビルドし、既存private guestとのABI一致を確認して署名する。現在のbuild5と同じセーブ／画質で、静止→視点操作→攻撃／魔法→同じ水域への潜水を比較。保存・ホーム移動によるcache checkpoint後の再起動と初回を分け、FPS平均だけでなくframe gap・停止区間・熱状態を見る。English初期値、Languageの即時変更／再起動保存、記号と複数指／メニュー移動も確認する。本人の端末操作は今は依頼しない。

---

以下はbuild5以前の作業履歴。

**更新 2026-11-14：本人の「ビルドして」でgo待ちを解除し、build5を実機へ更新済み。** 以下の未ビルド・未導入表記は各作業時点の履歴。導入時点では実機評価待ちだった。現在の本人報告と次回候補は上の最新欄。追加のクラウドジョブは起動しない。

## build5のApple検証・署名・USB更新

最初のCodemagic build14（`0308342`）は2分6秒でAPC fixtureのApple SDK `memmove`マクロ衝突により停止。本番処理は変更せず、計数区間だけmacroをpush/undef/popするよう修正。既存macroがある旧新版の再現条件も通常／ASan/UBSanで通過した。

修正版[Codemagic build15](https://codemagic.io/app/6ad977fcf240708df6941076/build/6af76bd490e83b09c4f9effe)（公開`b757c2bd991745638b87c544c1b3df6625c84674`）は8分40秒で成功。Macの36検査中34件成功、Windows専用cross-compilerと任意のhost FFmpeg実decoder検査2件は既定skip。実decoderのローカル比較結果をApple上で再実行したとは扱わない。Appleの実event/QoS、GPU resource待機、APC、shader同時要求等の検査を通過。Metal Toolchainを公式componentで導入した後、自作shader3件のiPhoneOS compileも成功。本体はarm64／iOS最低18.0／SDK26.5でcompile/linkした。

公開host SHA256 `9e229974bd627339bec9931669cd353f0ee528b55c20e459e935a0cb2182edd9`。`x86.h`と`PcGuestABI.h`はprivate guestのcompile contractに一致し、`rt_table.h`は既存late binding宣言の1行差だけでbuild13と同一。private guestの再コンパイルは不要だった。PC内で組み立てたunsigned IPA SHA256 `5e604ad3ce0eabc316a40d34eecf60b1f35da30dcc2f6876d1f9b5ece05cfce5`、署名済みSHA256 `c8f048e1d75fdfd22182405c18a826983899af8702d17caaa21b22136f24aec5`。2イメージ／39,413コードページ／4resourceと既存profileのApple chain・App ID・対象端末・3メモリ権限を検証した。

USB接続を再確認後、本人が保存・終了したbuild2をbuild5へUpgrade。前後のゲーム26ファイルの名前／サイズ（5,909,061,966 bytes）と、プロフィール6ファイルのSHA256（9,715,471 bytes）が一致。ゲームの全byteを今回再読み戻したという意味ではない。成果物／原ログは`local/codemagic-title15/`、プロフィールは`private/title-build5-before/`と`private/title-build5-after/`。原ゲーム、旧IPA、private guestは不変。OSのインストール受理は確認したが、ゲーム動作・画質・音声・FPS・発熱・初回cache保存の実機評価は本人の次回試遊待ち。

## 2026-11-14：シェーダーの重複準備と読み込み完了通知

追加5件の後、「やっていこう」に対して公開ホストの準備処理を追い、2件を実装した。実ゲーム由来のコードや新しいprivate guestは作っていない。比較基準は`737ee80`で、ここで変えた2経路はその時点では固定上流の処理のまま。

**同じD3D9シェーダーの同時コンパイルを共有。** 固定DXVKの`D3D9ShaderModuleSet::GetShaderModule`は、cache miss後にmutexを外してコンパイルするため、同じkeyを複数threadが同時に要求すると全員がコンパイルし、登録時に重複を捨てていた。既存のstage＋SHA-1 keyと完成済みmoduleのcacheは維持し、未完成の同一keyだけを待ち合わせる。公開実methodを組み込んだ自作compiler fixtureでは、8同時要求のコンパイルが8→1回になった。異なるkey／stageと完成済みmoduleは、先行compilerを停止させたままでも処理できる。

pending記録はmodule setごとに持ち、mutexで保護し、compiler呼出し中にmutexを保持しない。共有記録はmapのrehash／erase後もwaiterが保持し、完了・例外後に起こす。失敗はcacheせず、待機側は自分の入力で再試行する。同一threadから同じkeyへ再入した場合と、任意の待ち合わせ用確保に失敗した場合は元の独立コンパイルへ戻る。compiler失敗・全員失敗・登録時の例外・2つの確保失敗・再入・独立device/set・8thread×1,000要求を検査した。shader解析、stage／model検証、コンパイラー引数は変更していない。初めて見る単独shaderのコンパイルを消す変更ではなく、cold requestにはpending記録の確保が加わる。同一keyが失敗し続ける場合は各callerの再試行が直列になるため、待ち時間が常に短縮するとは主張しない。

実機での出現を測るため、既存5秒単位の`IOS_TIMING`へ`d3d_shader_wait`を追加した。同一keyの完了を待った区間だけを計測し、hot cache hitには時計読み取りを追加しない。samplesは待ち区間数であり、失敗後の再待機は別区間。GPU時間／コンパイル回数／cache命中数ではない。`failures_known=0`を出し、待ちが終わったことをcompiler成功と取り違えない。既存の数値kindをずらさず末尾へ追加している。

**完了通知の詰め直しを削減。** `ReadFileEx`／`WriteFileEx`の64件TLS queueは、先頭を取り出すたびに残りをmemmoveしていた。固定長ringにし、取り出し前のFIFO順、呼出しthread、callback実行前の件数減算、入れ子の通知、null callback、満杯時の元の失敗位置を維持した。64件を排出する際の詰め直しは2,016要素＝32,256byte→0。実際のread／write、OVERLAPPEDへの書込み、callbackへ渡す値は維持する。これはディスクI/Oの非同期化ではない。

公開実shim＋自作I/OをLinux x86_64の1CPUに固定し、旧新の順を交替した7ペアで比較。1／4／16／64件単位の通知処理は、1callback当たり14.858→13.242、12.410→9.727、15.113→10.488、19.356→9.441nsの中央値だった。各runの400万callbackのchecksumはすべて一致。実ファイルI/O時間・実機FPS・発熱を表す数値ではない。原値と再現scriptは`local/performance-20261114/pending-work-01/benchmark.json`と`benchmark.py`。0〜64件、周回、再入中の追加、full queue失敗、4つの独立threadを通常／ASan/UBSanで検査した。

**今回採用しなかった範囲。** `ReadFileEx`は依然同期preadであり、非同期化にはdescriptor／bufferの寿命とcaller-thread APC・取消の設計が必要。HLSLのinclude callbackはguest CPU/TLSへ戻るため、単純なworker移動・入力文字列だけのcacheを加えない。DXVKの`SetRenderState`は既に同値判定を持つ一方、その前にstate-block記録を行う。さらにbridgeで同値更新を省くと記録を欠落させ得るため追加しなかった。pipeline variant検索の新しいcacheも、この段階では実際の分布の根拠がなく追加していない。

新規fixtureの通常／ASan/UBSan、既存I/O計測・pipeline lock・時間分布の関連検査を通過。実際にstageした`d3d9_shader.cpp`、そのheaderを利用する`d3d9_device.cpp`、全adapter適用後の`kernel32.c`を本物の公開headerでsyntax検査し成功。CPU ABI header不変と公開source検査も確認。次回Mac用のfixture一覧とcache namespace入力に追加したが、scriptは実行していない。署名済みbuild4へは未収録。アプリ／SDK／private guest／クラウドbuild、push、署名、実機更新は行っていない。

## 2026-11-14：前回の3件に続く追加5件

本人の「さらに5回」に対し、`a313b61`を比較基準として以下の5件を採用。公開hostのみを変更し、原ゲーム・private guest・共有CPU ABI・画質設定は維持。Linux x86_64の許可CPU1個に固定し、旧新の順を交替した7ペアの中央値を比較した。実ゲームの利用頻度、iPhone FPS／発熱／瞬間停止改善は未測定で、下記の割合を合算してゲーム全体の改善率にはしない。

| 処理と自作条件 | 旧 → 新 | 削減 |
|---|---|---|
| CTAB定数表の構築、256定数 | 5.021 → 3.959µs | 約21％、定数vectorの確保9→1回 |
| ステレオ音声route、480frame | 2.815 → 1.076µs | 約62％ |
| 等倍point画像変換、512²、ARGB→ABGR | 2.637 → 1.347ms | 約49％ |
| BC1の4×4 block展開 | 56.227 → 39.135ns | 約30％ |
| shadow_flush、同objectの4subresource | 117.240 → 56.591ns | 約52％、作業queueの確保3→0回 |

**CTAB。** `make_table`で4096件以下・descriptor領域内という既存検証を通った後、既知の件数を一度reserveする。type/default/name検証、要素順、guest buffer、解放／登録は維持。予約は最大196,608byteで、途中の不正typeを拒否するより前に容量を予約する点は変更の性質。0/1/4096件、入力途中切れ、不正count/offset/最後のtypeで結果と解放を比較。16件370.52→307.51ns、1024件20.808→17.855µs。256件を2万回構築した全newは200,001→40,001回で、map node等は残る。前回の定数転送用一時配列とは別の処理。

**音声。** 2入力→2出力channelだけstrideと4つの乗算を明示。sum=+0、zero係数の乗算、加算順、volume/send順を維持し、左出力への加算後に右入力を読む。その他channel数は元loop。1〜8channel、既定／指定matrix、0〜513frame、欠損send/master/dead、signed zero/Inf/NaN、self-sendと1〜2floatずらした蓄積先を比較。有限値・Inf・signed zeroのbitは一致し、NaNはclassと件数を照合。最初の全byte比較では演算由来NaNの符号だけが異なったため、NaN符号／payloadのbit同一は主張しない（finiteの差は無視しない）。132構成の実engine_passはPCM全byte・callback・lock・voice寿命が一致した。非対象のmonoは2.357→2.382µs、6→2channelは5.101→5.138µsで、約1％の差を改善とは扱わない。

**等倍画像変換。** 同寸法のpoint系filterでは、元の`min(sw-1,x*sw/w)`等は厳密にx/yになる。完成済みRGBA snapshotをそのまま元のpackへ渡し、pixelごとの座標除算を除去。format変換、color key、lock前の全画像展開、same-formatコピー、拡縮/linear filter、temporary surfaceや失敗処理を維持。15形式の組合せ×4filter×各API失敗経路、部分block、実際に一致するcolor key、等倍/拡縮、非zero原点を実関数＋自作surfaceで比較。64²は38.254→20.441µs、1024²は10.581→5.622ms。先に試したscratchゼロ初期化の省略は512²で約3.5％悪化、1024²で改善と安定せず棄却し、元vectorと初期化へ戻した。

**BC1。** 元と同じrgb565展開、整数の/3・/2丸め、c0<=c1時の透明色を維持し、paletteを一度BGRA4byteへ並べてmemcpy出力。unaligned出力を許容し、DXT2〜5は元のalpha/color loopへ渡す。自作4096block×5形式×幅高さ-1〜5の1,003,520ケースで、padding/sentinelも含めbyte一致。透明selectorと赤endpointを独立期待値でも確認。CPUでBC1を展開する経路だけが対象で、Metalが圧縮textureを直接sampleする場合のGPU高速化ではない。

**描画更新queue。** 最初の8件を呼出し内のaligned storageへ必要な分だけ構築し、9件目以降はvectorへspill。大きなstd::function capture自体の確保は残る。snapshotをmutex内で作り、同じ順でmutex外実行し、例外時は構築済み要素だけを破棄する。0/1/2/4/8/9/16/33件、lock中subresource除外、再入、callbackによるregistry破棄、1/5/9/12個目のcopy例外、既存の並行shadow fixtureを検証。1件39.108→22.485ns、8件185.365→100.313ns、16件330.198→281.259ns。空は8.777→10.120nsと約1.34ns増え、全条件で速いとは主張しない。inline部は8*sizeof(Upload)のstackを使う。最初のoptional配列案は空8.315→30.989ns、1件38.448→41.299nsへ悪化したため棄却した。

新6fixtureは通常／ASan/UBSanで成功。関連する既存shadow、constant、音声timing observer、mobile/CPU ABI検査も成功。実shader/xaudio2/surface/bcemu/bridgeの5 translation unitを固定依存headerでWSL syntax検査し成功。新fixtureは次回Mac検証scriptに登録しただけで、Apple SDKによるcompileや物理iPhone上の音声・描画確認は未実施。

原数値と実行scriptは`local/performance-20261114/five-gains-01/`。CTAB/BC1の採用値は`benchmark-initial-candidates.json`、修正版route/surface/uploadsは`benchmark-final-changed.json`。初期JSONのsurface/uploadsは棄却案なので製品効果へ混ぜない。fixtureはhash照合した実公開関数と自作buffer/deviceを使う。署名済みbuild4は保持し、アプリ/SDK/クラウドbuild、tag/push、署名、端末更新は行っていない。

## 2026-11-14：比較で削減を確認した追加3件

本人の「明確な効果のある積み重ねを3回できるまでは続けて」に対し、前回の`f11fba7`以降に以下の3件を採用した。ここで確認した効果は公開実処理の操作数・PC上の所要時間であり、実ゲームの出現頻度、実iPhoneのFPS／発熱／瞬間停止改善ではない。Linux x86_64の許可CPU1個へ固定し、旧新の順を交替した7ペアの中央値を用いた。原数値／再現script／途中で不採用にした比較は`local/performance-20261114/three-gains-01/`に保持。

**1. 消費される型だけのシェーダー定数。** 前回はfloat／int両配列を作っていたが、`set_registers`が読むのはどちらか一方だけ。選択された型のゼロ初期化・数値変換・格納のみを行い、もう一方を作らない。16要素以下は前回の呼出し内配列を使い、大きな更新はvector 2個→1個。実際に渡す値、padding、register数、mutex、戻り値、guest ABIは維持。37,642ケースの実変換関数比較、独立した行列期待値、再入時の外側payload保持、512並行呼出しを通常／ASan/UBSanで再確認した。範囲外float→int等、上流に元からある未定義変換の全般的な修正を主張しない。以前の「両方の変換を維持」「大きい場合2個」という記録は前段の履歴であり、今回この点を更新した。

比較基準は前回採用済みの小配列版`f11fba7`。1register scalarは96.17→86.37ns、4register matrixは139.65→135.35ns、5／16／256registerの条件は155.56→148.18、158.70→142.88、345.04→233.91ns。30万呼出しで大配列の確保60万→30万回、小配列は0回のまま。結果は`benchmark.json`のconstant項目。転送された結果を捨てたり更新を間引いたりする変更ではない。

**2. キューブDDSの繰り返し計算。** 上流の`native_d3dx_dds_level`は要求面に到達するまで、それ以前の面の同じmipサイズを繰り返し計算していた。キューブ専用の`skyrim_ios_dds_cube_level`では最初の完全な面のレイアウトを一度だけ検証し、先行する面はchecked arithmeticで飛ばす。要求mipより後の未格納データを理由にfirst faceを誤って拒否しないよう、first faceは元関数へ渡す。データや画素には触れず、同じoffset・幅・高さ・深度・pitch・行数を返す。途中切れ・overflow境界・失敗時に出力を書き換えない性質も維持する。

256組の自作metadataについて、2D／cube／volume、BC8/16byteと非圧縮、base pitch、最大15mip、全face／level、必要byteの直前／ちょうど／直後、SIZE_MAX付近、null／不正指定を旧新比較。8×4 BC1の独立したoffset期待値も確認した。通常／ASan/UBSanを通過。自作6面×4段の全level照会ではサイズ計算300→90回。最初の汎用置換案は通常2Dで約15％悪化したため不採用。最終版では元関数の本文と2D／volumeの2箇所の呼出しを全文維持し、cube生成の呼出し1箇所だけを別APIへ接続した。維持範囲はfixtureで確認している。

最終の自作1024²／11mipでは、6面全照会が8,582.55→3,297.19ns（約62％短縮）。通常2Dは同じ元関数の別名同士の比較で289.99→291.39ns（約0.5％差）で、ここを高速化したとは扱わない。GPU転送・ファイル読込み・画像展開はこの時間に含まれない。最終比較は`dds-cube-only.json`。`benchmark.json`の初期DDS項目と`dds-refined.json`は不採用／途中候補で、製品に使わない。

**3. Directory cacheの検索対象削減。** 名前が多数あるdirectoryでは、毎回すべてのNUL終端名を辿り、strlenとstrcasecmpを繰り返していた。既存の不変listingに`{offset,length}`の8byte索引を追加し、queryとbyte長が同じ名前だけを調べる。元のnames配列は並べ替えず、索引の同長要素は元offset順にするため、大小文字だけ違う複数名では元と同じ最初の候補が返る。判定は元のstrcasecmpのまま。Appleの[公開実装](https://raw.githubusercontent.com/apple-oss-distributions/Libc/main/string/FreeBSD/strcasecmp.c)と[iOS libcの仕様](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/strcasecmp.3.html)で、対応するbyteごとの比較・現在localeの利用を確認した。独自の小文字化やlocaleに依存した検索結果の保存はしない。

mutex、各lookupでのstat、mtimeとnanosecond照合、明示forget、16slotの置換順を維持する。slot再利用時に古い索引も解放し、listing構築完了後に一度だけ索引を作る。32件未満、65,536件超、namesがUINT32_MAX超、追加malloc失敗では元の線形検索へ戻る。optional mallocがerrnoを変えないようにする。索引は最大512KiB／slot、16slot全部が上限なら8MiB追加で、namesの複製はない。今回の1,024件では約8KiB。既存listing確保失敗やmtimeを偽装された変更への上流の挙動を新たに解決したものではない。

実directoryを使い、hit／miss／混在case／日本語・非ASCII byte、comp出力0〜12byte、mtime変更・明示forget、新規file、20directoryによるslot置換、索引確保失敗、4thread×1,000回を検査。自作同長のcase重複では索引を実際に作り最初の名前が返ること、上限超でfallbackすることも検査。通常／ASan/UBSanを通過。512件以上のlistingで100回の存在しない長さの照会は、旧5万回超のstrcasecmpが新0回になった。保存データ／ゲームの内容はfixtureに含めていない。

2万回のhit／miss交互lookup（statを含む）の中央値は、16件1.288→1.243µs、128件2.085→1.190µs、1,024件11.329→1.200µs、4,096件47.769→1.410µs。同じ長さの名前だけを1,024件並べた条件でも11.674→6.103µsだった。初回cache構築は1,024件で約473→548µs、4,096件で約1,688→1,934µsと増え、頻繁に変わるdirectoryや一度しか調べない場合には利益を保証しない。値は`benchmark.json`のdirectory項目。実iPhone APFSやゲーム内のfile探索の計測ではない。

3件とも固定公開sourceへのadapterとして適用し、上流を直接変更していない。実`d3dx_shader.cpp`／`d3dx_image.c`／`d3dx_native.cpp`／`kernel32.c`全translation unitを既存headerでsyntax検査した。新規DDS／directory fixtureは次回Mac検証scriptへ登録しただけで実行していない。公開pin検査・共有CPU header不変・公開source検査も確認。private guest・原ゲーム・セーブ・署名済みbuild4を保持し、アプリ／SDK／クラウドbuild・新署名・端末更新は行っていない。

## 2026-11-14：シェーダー定数の小さな一時配列

固定公開runtimeの`d3dx_shader.cpp`にある`set_value`は、scalar／vector／matrixをD3D9定数へ変換するたびに、floatとintのvectorを各一つ作り、両方をゼロで初期化していた。`patch_constant_scratch`で、16要素以下（float4 register 4個＝行列1個）の場合だけ呼出し内の固定配列へ変更した。float／int配列は計128byteで、thread共有やTLSに置かず再入時も別の領域になる。17要素以上は元と同じ2個のvectorで必要量を確保する。ゼロ初期化の範囲、両方の数値変換、guestからの読取り、index上限、転置・pointer array、deviceへの引数・戻り値、mutexの位置は変更していない。大きなconstant arrayの省略や精度削減はしない。

実際の`Constant`／`Table`、handle解決、`set_registers`、`set_value`と全Set wrapperを、自作guest引数／同期device sinkへつないで旧新比較した。vertex／pixel、bool／int／float、4種類のclass、1〜4行／列、count 0／1／3／4／5／16／256、scalar即値／array／vector／matrix／転置／pointer array／raw、ゼロ余白・符号付きゼロ・部分入力、name／数値handle、null入力、欠損table／handle、device失敗を含む37,642ケース。戻り値・guest stack更新・API種別・register範囲・転送byteを比較し、旧新ともdigest `17909396858461555299`、device呼出し32,776回で一致。行列の正順・転置には独立した期待値もある。device callbackから再入した後も外側のpayloadが変わらず、2thread×256回も実recursive mutex下で成功した。正常範囲の入力が対象で、既存のfloatからintへの範囲外変換等の未定義動作を解決したとは扱わない。

通常／ASan/UBSanを通過し、小さな呼出しの確保2→0回、大きな呼出しの確保2→2回を実operator newで計測した。固定public dependency headerを使った`d3dx_shader.cpp`全体のWSL syntax compileも成功。検査用include指定には最初`vkd3d_types.h`の探索先が欠けていたが、既存の固定vkd3d/includeを指定して解消した。新しいdependencyの導入やupstream変更ではない。

Linux x86_64の許可CPU1個で30万呼出し×旧新交互7ペアを比較。実変換関数と自作device sinkを含む1呼出しの中央値は、1register scalarが112.32→95.04ns、1register vectorが128.64→115.40ns、4register matrixが165.69→139.12ns。各30万回のheap確保は60万→0回。大きな5／16／64／256register条件は各60万回の確保を維持し、時間は167.10→154.97、167.58→159.48、185.32→171.83、329.72→309.89nsだった。後者の短縮は実行コードの配置・最適化の影響も含み、allocation削減による効果とは説明しない。全条件は自作CPU負荷であり、実GPUの更新・実ゲーム頻度・iPhone FPSを測っていない。元数値と再現scriptは`local/performance-20261114/constant-scratch-01/benchmark.json`、実source検査は同directoryの`shader.syntax.log`へ保存。

追加調査ではDXVKの`SetShaderConstants`／`UpdateStateConstants`も確認した。同値時のupload省略には、state blockの記録、使用register範囲、`d3d9FloatEmulation`のNaN補正、boolのbitfield更新を考慮する必要がある。全呼出しへ無条件のmemcmpを加える根拠となる重複頻度もまだないため、その変更は採用していない。既存のdirty state／描画命令／GPU同期を削っていない。

公開adapterの16pin・共有CPU header・公開source検査を確認。新fixtureは将来の一括Mac検証scriptへ追加したが、scriptは実行していない。private guest・既存IPA・元ゲーム・セーブは不変。この変更は署名済みbuild4に未収録で、goまでアプリ／SDK／クラウドbuild・署名・実機更新を保留する。

## 2026-11-14：GPU resource完了待ちのCPU消費削減

固定DXVKの`DxvkResource::waitIdle`は、resourceの読み書きカウンターを繰り返し確認し、50,000回ごとにschedulerへ譲る方式だった。D3D9の`WaitForResource`は必要なflush／CS同期の後にこれを呼び、GPUの完了後にlifetime trackerがresourceをreleaseする。Appleの[CPU作業の効率的なスケジューリング](https://developer.apple.com/documentation/xcode/scheduling-cpu-work-efficiently)も、pollingを減らし通知を使う原則を説明している。実ゲームでの待機頻度は未測定で、この箇所がカクつきの主因だとは断定しない。

採用候補は200回だけ短く確認し、まだ使用中ならcondition variableで休む方式。200は既存DXVK小規模spinlockの境界を使用し、機種別の測定済み最適値とは扱わない。resourceごとにmutexを増やさず、128byte境界の固定64bucketへaddressを割り当てる。tableはstatic領域へ一度だけ構築し、終了時destructorを登録しないため遅いdriver teardownとも衝突しない。heap確保・resource pointerの保存はなく、同じaddressが再利用されても古いresourceへの参照を持たない。resourceのmember／サイズ、元のacquire・release・loadのseq_cst順序、Readがread＋writeを待ちWriteがwriteだけを待つ規則は維持する。

待機登録と条件確認を同じbucket mutex内で行い、完了側の通知もこのmutexを取得する。登録前に終わった場合は後の条件確認で完了を観測し、条件をfalseと読んだ直後に完了しても、通知がmutexを経由するためsleep開始との間で失われない。複数resourceのbucket衝突や余計な通知は元のカウンター条件を再確認する。release側は待機者がいない場合はglobal atomicの読取りだけで抜け、対象accessが未完了なら通知しない。待機者が別resourceにいる場合の追加負担も下記で比較した。

既存の5秒診断に`IOS_TIMING kind=resource_wait`を追加。最初の200回の確認後から、bucket準備・lock・条件確認／待機・復帰までのhost wall timeを記録し、bucket lockを解放してから集計する。完了と競合して実際にはsleepしない呼出しも含む。GPU実行時間、kernel sleep回数、D3D9のflush／CS同期を含む全待ちではない。失敗を観測するAPIではないので`failures_known=0`。他のPresent等の計測と時間が重なるため合算しない。

検証は本物の公開resourceクラス／implementationと自作の完了通知を使用。Read／Write／None、登録前の完了、false条件読取り直後の完了、同bucketの別resource、複数waiter／複数producer、データ可視性、同addressの128回再利用、旧新resourceサイズ一致を確認。通常とASan/UBSanを通過した。ThreadSanitizerは`unexpected memory mapping`で起動自体が失敗したため、race検査成功には数えない。実ヘッダーを使用した`dxvk_resource.cpp`、`dxvk_lifetime.cpp`、`d3d9_device.cpp`全体のWSL syntax検査も成功。追加分類を含むmetricsの境界・80,000並行記録／drain検査は通過した。実Metal／Apple condition variable／iPhoneのゲーム結果は未確認。

局所比較はLinux x86_64で許可CPU2個に固定、旧新の順を交替した7ペア。完了側は自作threadでありGPUではない。下表は各ペアの1回当たり時間の中央値。thread生成・schedulerの影響を含むため、設定したsleep時間とwall timeは一致しない。完了後遅延はproducerがrelease直前に時刻を記録してからwaiterが戻るまでで、OSの純粋な起床時間ではない。

| 自作負荷 | 旧→新 待機thread CPU時間 | 旧→新 完了後遅延 |
| --- | --- | --- |
| 50µs sleep ×64回 | 430.4→47.3µs | 0.59→40.46µs |
| 2ms sleep ×16回 | 2.229→0.078ms | 0.68→70.13µs |
| 10ms sleep ×8回 | 10.249→0.085ms | 1.26→74.03µs |

2ms条件の全wall timeは2.268→2.304ms、10ms条件は10.306→10.326msで、CPU消費削減と復帰遅延の交換がある。待ちなしのacquire／release／waitIdle 300万回は15.38→14.86ns／回。別resourceを待つthreadがいる時は15.73→17.46ns／回で約1.74ns増える。最初のactive比較は二つのresourceがcache lineを共有していたため、最終比較は各128byte境界へ分離して再測定した。iPhoneのFPS・発熱改善率には換算しない。最終値は`local/performance-20261114/resource-wait-01/benchmark.json`と再現script、実source検査ログも同directoryに保持。

次の実機確認では同じsave／画質／FPS上限で、`resource_wait`とframe gap／Presentの分布、魔法・攻撃時の引っ掛かりを確認する。待機が多くてもGPUそのものが速くなる変更ではなく、短い待ちに対する起床遅延やbucket競合は実機で判断する。新fixtureを将来の一括Mac検証へ組み込んだだけで、scriptは未実行。アプリ／SDK／クラウドbuild、private guest再生成、署名、実機更新は行っていない。既存の署名済みbuild4には含まれない。

## 2026-11-14：同期音声展開の確保回数と観測

固定公開runtimeの`xs_SubmitSourceBuffer`は、xWMA展開を音声mixerと共通の`g_lock`保持中に同期実行する。その中の`receive_pcm`は復号frameごとにPCMの`realloc`を呼ぶ。ロックを外すとvoice破棄・reset・queue変更との整合性が必要になるため、今回はロック／再生順を維持し、PCM出力bufferの確保だけを変更した。元ゲーム関数やprivate guestは変更していない。

`prepare_runtime.py`で`xwma.c`をLF正規化SHA256で固定し、上流を編集せずstage時に適用する。decode呼出し内だけでcapacityを管理し、残り入力packetがある時のみ余裕を確保する。最大必要量を超える先行確保分は64KiBまで。先行確保が失敗したら旧版と同じ必要量で一度再試行し、任意の再試行／縮小がerrnoを変えないようにする。終了時は旧版の最終要求量へ縮小し、縮小失敗だけを理由に正常PCMを失敗扱いしない。callerの`free`所有権、decoder reservoir、packet/PTS順、resampler設定、音質、完了／reset／失敗時cleanupは維持する。

初期案は1packet単位のstreamingで余計な確保を2回増やしたため、そのまま採用しなかった。残り入力がない時に先行確保をしない方式へ絞り、最終自作fixtureではstreaming部分の確保回数が旧新とも9,120回。全9,138 decodeケースでサンプル／戻り値／frame数／PTS／終了・reset／errnoが一致し、確保呼出しは18,290→10,824回。常にbufferを移動する自作allocatorではコピー9,866,388,768→2,840,088,164 bytes。これは強制移動時の処理量比較で、実mallocの移動頻度やゲームの処理時間ではない。1〜8ch、複数packet、zero-output、EOS後の再利用、codec失敗、確保・縮小失敗、frame数overflowを含む通常／ASan/UBSan検査を通過。

実FFmpeg 6.1.1と一致するUbuntu公開development packageは`local/performance-20261114/xwma-01/headers`へ展開しただけで、OSへのpackageインストールはしていない。自作WMAv2 toneの1ch/22,050Hz/0.3秒、1ch/44,100Hz/3秒、2ch/48,000Hz/12秒について、一括／1packet streaming・EOS後再利用で旧版／新版とFFmpeg CLIの16bit PCMがbyte一致した。ゲーム音声、WMA Pro実stream、iPhoneの音声出力はこの検証の対象外。

実C decoderを同一許可CPUへ固定し、順番を交替した7ペア×40回で比較。一括展開の確保回数は6→4、64→8、281→35回、streamingは6／64／281回のまま。最終12秒／2ch一括の中央値は旧6.709ms、新6.714msでほぼ同じ。短い条件やstreamingの時間は比較回ごとの揺れがあり、全体高速化を断定しない。採用候補の根拠はallocation／移動時copy操作の削減と意味の保存であり、実機のFPS・瞬間停止・発熱改善は未測定。原比較は同local directoryの`benchmark.json`（初期のPython込み計測）、`benchmark-c-pinned.json`（最終C計測）と再現scriptに保持。

次回の原因識別用に`IOS_TIMING kind=xwma_decode`を追加した。既存の5秒histogram出力を使い、PCMやファイル名を出力しない。対象は呼出し内のhost wall timeで、mixer lock取得待ち、音声GPU/DSP時間、画面の停止時間ではない。従来のmutex位置・全引数・戻り値・errnoが変わらないことを実adapterから抽出したwrapperで確認し、ASan/UBSanも通過。共有metricsの境界・並行drain検査、実`xaudio2.c`全体の既存headerによるsyntax検査、CPU ABI不変・mobile overlay検査も通過した。

新fixtureは次回のMac検証scriptへ加えたが、そのscriptは実行していない。FFmpeg実比較は明示的なhost開発環境がある時のみ実行し、未設定時はskipを成功と混同しない。アプリ／SDKビルド・新しいIPA・署名・端末更新は未実施。既存build4を上書きせず、この追加分は次回ビルド候補として保持する。

ユーザーは20〜21時ごろまで実機を接続できない。画質やゲームの結果を維持し、処理量を減らせる変更を比較して採用する。サブエージェントなし。私有ゲームコード・素材・ログ・署名はPC内のみ。旧build2/3と元ゲーム・セーブを保持。Macビルドは20時以降に検証済み変更を一度まとめる。21時以降は新規の長い実験を始めない。14時台の本人指示で`skyrim-metal`の自動送信を削除し、瞬間的なカクつき対策をgoalで管理する方式へ変更した。定期送信は再作成しない。

## 現在の基準

- root `codex/ios-title-milestone`、build3コード `118ddbb`、結果記録 `7a4f150`。
- 公開ミラー `local/prior-preflight` の `codex/skyrim-pc-aot-foundation` は `b46dda7`。build3実SDKコードは `0239ba2b7becbddc73a6914c95425d34be8a0295`。
- build3署名済みIPAは `private/title-signing-20261112/SkyrimIOS-PC2011-build3-signed.ipa`。未インストール。詳細はSTATUS。
- 原ゲーム処理90,390関数は元の処理を静的変換している。private guestは `-O1 -fno-builtin -DLB_STRICT_ORDER`。画質・浮動小数点精度・同期保証を落として速度を得る方針は採らない。

## 採用した変更

1. **strict-orderの2/4バイト書き込みの退避削減**：`0006-inline-ordered-stores.patch`は`TSO_PAIR`のunaligned書き込みだけを明示的builtinにする。aligned release atomic／unaligned release fence、CPUレイアウト、浮動小数点、生成されたゲーム処理はそのまま。実際のClang19・iOS ARM64・既存フラグで、helperのstack退避／再読込がなくなり、`stlr`／`stlrh`・全`dmb`を維持することを確認。元／修正後それぞれ524,288件のbyte oracle比較を通過。x86でARM helperの分岐本体を選んだ値の検査と、実ARM機械語の検査は別物であり、実機の並行動作やFPSの証明ではない。
2. **D3D9永続テクスチャ管理の走査縮小**：`shadow_flush`と`shadow_forget`を`lower_bound({obj,0})`から同objectの範囲だけ走査する。callback順序、ロック、所有権、upload条件は維持。実際の旧／新shadowコードで、内容・subresource順（UINT32_MAXを含む）・lock中の除外・再入callback・解放順・無関係object・並行flushを比較し、ASan/UBSanも通過。flushの呼出し元は`UpdateTexture`／`UpdateSurface`であり、全drawごとに同処理が走ると仮定しない。
3. **Vulkan pipeline cacheの保存・再利用**：既存のDXVK state cacheは維持し、それとは別にMoltenVKのpipeline cacheへ前回保存したデータを渡す。保存はアプリが背景へ移った時のutility workerだけ。通常の描画中にファイル保存／cache全体のシリアライズを行わない。GPU・driver・OS・公開変換器の版を照合し、破損・不一致・容量超過なら空cacheで開始する。詳しい根拠と制限は下記。実iPhoneの再コンパイル時間やFPS改善は未測定。
4. **固定サイズの時間分布計測**：5秒ごとの既存FPSログに、frame間隔・Present内部のhost時間・追加FPS待機・HLSL／graphics pipeline／compute pipeline／shader module作成の分布を追加。histogramの集計は固定配列とatomicだけで、毎frameのログやallocationはない。frame間隔と復帰時のリセットだけは短い専用mutexで整合させ、その中で描画・UIKit処理は呼ばない。FPS上限のsleep回数も分離して記録する。GPU実行時間やCPU使用率として扱わない。
5. **音声submixの対象探索縮小**：各passで64stage×1,024voice枠を走査する処理を、同一mutex内の安定bucket列挙へ置換。stage昇順と同stage内の元slot順を維持し、実際の音声計算・callback・解放順は変えない。毎pass再構築するため古いvoice graphを再利用せず、heap allocationも追加しない。
6. **診断slotのwriter分離**：毎importで更新するthread別記録を128byte間隔へ配置。記録本体・頻度・dispatchの挙動はそのまま。64slotのstatic領域がx86_64で3,072→8,192 bytesになる。並行書込みのfalse sharingを減らす目的で、診断そのものやgame synchronizationを削除しない。

WSL x86_64 GCC -O2でのshadow局所比較（7ペアの中央値、1objectにつき4subresource、実guest変換を別スレッドで実行中）：16objectでは約436→246ns、256objectでは約6,075→245ns、4,096objectでは約181,665→301ns。大量の無関係なmap要素を省けた実験であり、実ゲームでのobject数・呼出頻度・upload量を測った結果ではない。原数値は`local/performance-20261113/runtime/benchmark.json`。

## 棄却・保留した案

- **16バイトSIMD memcpyのbuiltin化は棄却。** 生成513 Cファイルの23,648箇所を見つけたが、実際のDarwinヘッダーではfortified memcpyが既にbuiltinへ展開される。自作7種類のSSE storeで旧／新ARM出力は全文一致。3,328件のnative SSE・全CPU状態・memory比較も一致したが性能改善にならない。実験は`local/performance-20261113/fixed-copy/`。`-fno-builtin`というフラグだけで外部呼出しと判断してはいけない。
- **heapの空bin bitmapは棄却し、productionのheap.cを原文へ戻した。** 同じ割当アドレス・payload・再割当・coalescing・高位bin・失敗・60,000操作／30,000並行alloc/freeとASan/UBSanは通過。一方、空binや上位binでは速くても即時fitで遅くなる。最初の案を2百万組×7ペアで測ると即時fit約2.5%遅く、先頭binを優先する改案でも約15.9%遅かった（並行cross-compileによる雑音を含む）。ゲーム内の分布を測らず総合改善を断定しない。候補の実C・harness・数値は`local/performance-20261113/runtime/`と`runtime-final/`に保持。公開の`test_shadow_ranges.py`はheap不変も確認する。
- **追加memory fence削減は実装しない。** 一部の`ST_FENCE`とrelease storeは局所的には重なるが、DMBは後続の複数アクセスへ、releaseは当該アクセスへ順序を課す。混在するSSE／raw memory経路全体を証明せず削れない。自作MOVUPS/MOVUPD/MOVNTPSでCapstone由来access情報と実store方向の不一致も観測したため、metadataだけで最適化しない。現在の全fenceを保持。

固定アドレスのmemory/string/physics置換は、以前の有限検証が全CPU flags/SIMD/並行処理を覆っていないため、現時点で丸ごと有効化しない。実機でCPU/GPU/シェーダーコンパイルの内訳を測る必要は残る。

## 実行状況

build4の公開sourceとprivate guestのローカル検証が完了。build3の操作・画質・FPS上限は継承し、画質は自動で変更しない。新しいクラウドビルドは未起動。

- `private/title-performance-20261113-01`でprivate guest再生成完了（378.83秒）。90,390関数・未対応510件は基準と同じ。生成515ファイルと共有ABIヘッダーは基準とbyte単位一致。原入力のハッシュ不変。新helperを含めた514 unitのWindows→iOS ARM64 compile/linkはexit0で完了（旧exec session72512は終了済み）。コンパイラー・全フラグ・共有CPU ABI・17個のsystem importは基準と同じ。コンパイルを再実行しない。
- 新frameworkは`private/title-performance-20261113-01/arm64-ios/SkyrimGuest.framework`、unsigned SHA256 `dedd20f0e42d4c24b10f7f10b193b5987c4bd45a069ac34e8cc263efe7c86d48`。150,660,464→149,123,672 bytes（1,536,792 bytes減）、実`__text`は142,685,512→141,170,928 bytes。データ領域のサイズは同じ。コードサイズ削減は測定事実で、FPSや発熱改善量ではない。
- 既存のハッシュ固定済みpolicyで、新frameworkから署名前の`arm64-ios/signing-content-binding.json`を独立生成済み。manifest SHA256 `1da3244849c8d83538c2fb06a6e15e82e81f5b112424adfa6f10708baaee46be`。署名の際はこの新guestと新manifestを使い、元build2/3の値を流用しない。
- 最終public runtimeの実shadow比較、TSO helper比較、Windows ARM64出力比較を通過。heap棄却後の最終shadow検査（heap原文不変も検証）、WSLのpatch contract 8件、公開source混入checkも通過。host runtime→CPU patch→binding宣言のステージ適用も確認済み。ただし以前の`local/performance-20261113/host-overlay`は棄却heap候補を含む実験stageであり製品に使わない。クラウドは最終public scriptから毎回新規stageを生成する。
- Mac用の新fixture実行ではworkflowの`IPHONEOS_DEPLOYMENT_TARGET`をsubshell内で外し、`SDKROOT`をmacOSに指定。Mac用test binaryへのiOS環境混入を防ぐ。shell構文確認済み。実Macでの新fixtureは一括ビルド時に確認する。
- `Info.plist.in`とassemblerはbuild4を要求。private signing/install/snapshot helperは`private/title-signing-20261112/*_build4.py`として準備済み。旧build3 helper/IPAは保持。署名・端末操作はまだ行っていない。
- 性能sourceはroot `7f12ae5425497281393c35f5a0905263125b6034`、公開ミラーは`a78312c2ba1236baa84eada9f69a0f7f3a50f8ef`として既存branchへpushし、remote読み戻しを照合済み。対象10ファイルのGit blobは両repoで全文一致。後続の結果記録commitはコードを変えない。legacy Xbox CIには変更なし。通常pushがMacジョブを開始しないことを既存設定で確認済み。

## 次の作業

1. ユーザーから「時間があるので他にも進めたい」と追加指示あり。下記の未着手候補を調査し、元結果を保てる変更だけを比較して採用する。完了した検査やprivate compileを根拠なく繰り返さない。端末のFPS・発熱・場面ごとのstall計測が必要な点は残る。
2. 20:00以降、内蔵ブラウザでCodemagicのFree残時間／他ジョブ／最新commitの既存実行を確認して、`pc2011-title-host`を1回だけ実行。ordered store／shadow range／時間分布／pipeline cacheの公開fixtureを同Macで実行する。Mac/Simulatorを実iPhone合格と混同しない。
3. 公開host build4を入手し、共有ABI照合後に上記新private frameworkとPC内で組み立て、既存profile/DPAPI手順で署名。assemblerの入力exeは同じ`private/discovery/basepin-english-1932-20261112-01/inputs/candidate.exe`、SHA256 `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`。
4. 本人が保存・終了・USB・ロック解除したらプロフィールのみ最小保護して2/3→4をUpgrade。ゲーム26ファイルは再コピーしない。未接続なら一度だけ準備を案内し、待ち続けて再質問しない。`skyrim-metal` heartbeatは本人指示で既に削除済み。

## 追加調査の結果（14時台）

- **cacheの欠落を確認して実装。** 固定DXVKの`dxvk_pipecache.cpp`は毎回`initialDataSize=0`から作り、保存していなかった。MoltenVK v1.4.0の[公式説明](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.0/Docs/MoltenVK_Runtime_UserGuide.md#shader-loading-time)と[実装](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.0/MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm)を照合。保存対象は圧縮MSLと変換条件などで、warm起動のSPIR-V→MSL再変換削減が期待できる。Metal pipelineの生成全体や、初回の未遭遇effectの処理まで消えるとは言わない。既存DXVK state cacheと併用する。
- `ios/dependencies/prepare_dxvk.py`は固定6ファイルのLF正規化SHA256を確認して、公開sourceの別stageを作る。vendor／CPU変換／source-lock／private guestは変更しない。CMakeはこのstageをコンパイルし、source-lock・依存ビルドscript・SPIRV-Cross patch・runtime/DXVK adapter・cache実装・compiler/SDK指定からcache namespaceを作る。
- `pipeline_cache.cpp`はGPU vendor/device/driver/API/UUID、OS、namespaceを完全一致で照合する。payloadは64 MiB上限、固定名の公開済みファイル1個＋未公開pending1個。checksumは偶発破損検出用で署名ではない。Vulkan V1 headerも確認し、形式・サイズ・key・内容が違うファイルはdriverへ渡さない。driverが受け付けない時も空cacheで1回だけ再試行し、空cache作成自体の失敗は元のDXVKエラーとして残す。
- 保存はbackground通知時のutility workerで実行。先行するWillResignActiveで[Appleのbackground task](https://developer.apple.com/documentation/uikit/uiapplication/beginbackgroundtask(withname:expirationhandler:))を取得し、完了／復帰／期限切れで必ず終了する。期限切れ／復帰は後続処理をキャンセルし、fsync＋renameが完了するまで旧cacheを保持する。実行中のdriver API／fsyncを強制中断する機能ではない。MoltenVKのcache lockはシリアライズ中にcompilerと競合し得るため、foregroundでの定期保存は追加しなかった。
- cacheは元の`flags=0`によるdriver内同期を保つ。registryはcheckpointと破棄の寿命だけを守り、pipeline作成はそのlockを取らない。保存中に新しいcompileが終わっても世代カウンターにより未保存扱いを維持。既存DXVKのworker停止→pipeline破棄→cache破棄→device解放の順も維持する。
- **検証。** 新cacheの実C++ owner／file処理を自作Vulkan関数で実行し、同じpayloadの再読込、版/GPU不一致、破損/切断、リンク拒否、driver拒否後の空cache、容量上限、VK_INCOMPLETE、取消、保存中の世代更新、checkpoint中の破棄を確認。通常実行とASan/UBSanを通過。時間分布の境界・失敗数・80,000並行記録のdrain合計、固定上流へのadapter適用も通過。変更されたDXVKの5 C++ translation unitはWSLで実ヘッダーを用いたsyntax compileが成功（上流のstd::iterator非推奨警告だけ）。実Metal cache保存／UIKit lifecycle／iPhone効果はまだ未検証で、20時の一括ビルドと実機で確認する。
- 追加host機能のコードcommitはroot `f878339c715ae51ec9ab34649aa3a3e1113a2dd9`、公開ミラー `6e4f1565a112d56f663f224b4839e2cc9b8e1861`。対象19ファイルのcanonical Git blob一致・source混入check・push後のremote読み戻しを確認済み。後続docs commitはこのコードを変えない。新Macジョブとprivate compileは起動していない。
- **フレーム待機は変更していない。** 追加capは最大4ms sleep＋UIKit処理で、未設定なら待たない。固定DXVKの追加maxFrameRateも既定値0。VSyncの待ちはPresent内のsubmission同期／swapchain取得に別途あり、設定capを使う場合に両方の待機が生じ得る。ただし現端末のbuild2に追加capはなく、現在の引っ掛かりを二重capのせいと断定できない。今回の`pacer`時間・sleep回数と`present_host`時間で、帰宅後に切り分ける。入力応答や表示の同期を犠牲にする変更は保留する。
- **CPU処理の追加最適化**。現在のprivate guestはO1。O2や限定native置換は候補だが、コード量や合成速度だけで採用せず、flags／浮動小数点／並行処理を含む結果比較と実機の負荷根拠が必要。完了したbuild4候補を保った別実験として扱う。

### 新しいログの読み方

`IOS_TIMING schema=1`は5秒を目安にまとめ、`end_ns`と`window_ns`で実際の集計時刻・長さを示す（初回windowは0＝不明）。各binの上限nsは`0,1000000,2000000,4000000,8000000,12000000,16666667,20000000,25000000,33333334,40000000,50000000,66666667,100000000,166666667,250000000,500000000,1000000000,2000000000,UINT64_MAX`。最後は2秒超。binから求めたpercentileは範囲／上限値であり、正確な1フレームのp95ではない。

`frame_gap`は成功Present間隔で、backgroundを跨ぐ値は除外。`present_host`はPresent内のhost時間。`pacer`は追加cap処理のhost時間と`nanosleep`呼出数で、UIKit処理時間も含む。HLSL／graphics pipeline／compute pipeline／shader moduleの4種類は実API作成処理。追加の`graphics_compile_wait`／`compute_compile_wait`は同じshader組合せの生成mutexを即時取得できなかった時の待機時間。描画要求と先行コンパイルworkerの両方を含み、ゲームの描画スレッドだけの停止時間ではない。準備済みhitにはこの追加時計読取り／histogram更新はない。pipelineの再利用回数・実cache miss・GPU時間は測っていない。`failures_known=0`の種類について失敗0とは判断しない。atomicの読取境界をまたぐ記録があるため`approximate_window=1`とし、隣接windowを含めて評価する。5秒内の同時増加だけで因果関係を確定しない。

`IOS_PIPE_CACHE event=create offered_bytes=...`はdriverへ渡したbyte数で、命中数／受理済み数ではない。`event=checkpoint stored=1`は今回のファイル保存成功。実機確認は「同一save・同一画質」で初回→ホームへ戻り数秒待つ→完全終了→再起動を比較する。メモリー全体のステート保存やゲームのセーブを置き換える処理ではない。

参照：[Clangのmemory builtins](https://clang.llvm.org/docs/LanguageExtensions.html#memory-builtins)。固定サイズのcompiler builtinと通常のCライブラリー呼出しは区別し、実コンパイラーの出力で確認する。

順序の検討：[Armのacquire/release実例](https://learn.arm.com/learning-paths/servers-and-cloud-computing/memory_consistency/examples/)、[LLVM Atomic Guide](https://llvm.org/docs/Atomics.html)。これらは現在の全guest変換の正しさを保証する資料ではない。

## 瞬間的なカクつき：準備済みpipelineまで待たせるロックを分離

固定DXVKの`DxvkGraphicsPipeline`／`DxvkComputePipeline`は、instanceリスト検索とdriverへのpipeline生成を同じ`sync::Spinlock`の中で行っていた。state cacheのworkerが別state variantを生成している間も、同じshader組合せの準備済みvariantを描画側が取り出せない。Spinlockは200回の確認ごとにyieldし続けるため、長いcompiler処理にもCPU側の競合が続く。これはコードで確認した停止経路であり、現在の実機stallの主因と断定したものではない。

`prepare_dxvk.py`に以下を統合した。vendor、CPU変換、private guest、VulkanのAPI引数や生成コードは変更しない。

- instanceの検索／追加だけを短い既存spinlockで守る。準備済みhitは従来どおりこのlock一つでhandleを返す。
- 未準備の場合だけ別の`dxvk::mutex`（nativeでは`std::mutex`）で生成を直列化し、取得後に再検索する。同じshader組合せのcompiler数は増やさず、同じstate／render passの二重生成も防ぐ。ロック順は生成mutex→リストlockだけ。リストlockを保持したまま生成mutexを待たない。
- driver呼出し中はリストlockを保持しない。失敗したnull handleのキャッシュ、graphicsの不正state拒否、counter、state cacheへ追加する条件を維持する。初回の未準備pipelineは完成するまで待ち、描画を省略しない。
- 新instanceのhandleを生成mutex内で値としてコピーしてからstate cacheへ通知する。通知中に他の生成でvectorが拡張されても、元instanceへのpointerを後から読まない。
- 競合した生成mutexの待機だけを固定histogramへ記録。コンパイル自体の時間とは区別する。

**検証結果。** `test_pipeline_locking.py`は実際の旧／新4メソッド、実instanceクラス、実Spinlockを使い、compilerだけを自作fixtureに置き換える。別stateのコンパイルをgateで止めると、旧graphics／computeは準備済みhitも止まり、新版はgate解除前に返せた。未準備要求は待ち続け、同state同時要求の生成回数は1のまま。不正state、API失敗、例外後の再試行、render passの区別、callback中のvector拡張、graphics／computeそれぞれ16,000並行lookupを通過。通常実行とASan/UBSanの両方で成功。これは自作compiler条件下の停止解消で、MetalやゲームのFPS測定ではない。

実DXVKの5 translation unit（graphics／compute／pipeline manager／shader／pipeline cache）は実ヘッダー付きWSL syntax compileを通過。新stageは`local/performance-20261113/dxvk-locking-01`。新しい時間分類と既存wrapperのAPI引数不変、stage固定照合も検証した。Macの一括ビルドにもこのfixtureを追加した。private guestの再compileは不要。

**追加調査と残る制約。** Win32待機shimとDXVKのsubmission待機は既にcondition variableを使用。BSAの読込みもfd共有＋preadを実装済みで、単にpolling／毎回openだとは扱えない。一方、D3D9のresource readback／Lock時にはGPU使用中のデータを待つ経路と`waitIdle`のspinが残る。待機の除去やコピー省略は古いデータを見せる恐れがあり、今回は変更していない。compiler worker数の制限、GPU vendor偽装による全shader先行生成、fence削減も、実機での頻度・負荷根拠なしには採用しない。

[DXVK公式の開発者向け説明](https://github.com/doitsujin/dxvk/wiki/Developer-guidelines)も、初使用直前のshader生成がstallを起こすと説明している。ただしこれは新しいDXVK／D3D11向け資料であり、現行の固定D3D9＋MoltenVKへそのまま適用した証拠ではない。今回の採用根拠は固定コードのロック範囲と独立gate比較。初めて遭遇するeffectのHLSL／Metal生成、GPU負荷、熱による速度低下までは解消していない。

## 部分逆コンパイルによる候補選定と計測の追加

本人の「進めて」を受け、既存discovery／生成Cと元PEを使い18関数・316 blocks・2,125命令を限定解析した。候補をshader準備、texture準備、read submissionの3系統に絞り、擬似コード、TLS／global、入力所有権、callbackと失敗時の副作用、置換条件を`private/analysis/stutter-paths-20261113-01/CANDIDATES.md`へ保存。私有の関数対応表・逆アセンブルはPC内だけで保持する。元入力とdiscoveryは期待SHA256に一致し、ゲーム由来生成C／frameworkは変更していない。

- shaderには既存のキー検索／作成済みguardがあり、重複cacheを追加する根拠はまだない。初回のD3D shader生成はHLSLを通らない経路もある。一部のHLSL routineは直接の呼出元が不明で、実行頻度は未測定。全shaderを毎frame再コンパイルしているとは判断しない。
- textureはstreamの全読込み、条件付きのmip選択／再コピー、hostのdecode／uploadを分ける必要がある。元のmip policyを消す変更は画質を変えるため採らない。
- `ReadFileEx`のhost実装は同期`pread`後にAPCをqueueへ入れる。API呼出中の待ちは存在し得るが、ゲームworkerか描画threadかは静的情報だけでは不明。非同期化には要求threadでのcallback、fd/handle再利用、buffer／OVERLAPPED寿命、取消／終了、alertable waitの起床までが必要で、今夜の候補へ未検証のworkerは入れていない。

**追加計測。** `read_file`は既存ReadFileのread／pread部分、`read_file_ex`は既存read-exのpread部分（WriteFileExは対象外）、`d3d_shader_create`は実CreateVertexShader/CreatePixelShader呼出し、`texture_load`は実D3DXのmemory/file texture作成を測る。textureのfile版はhostの読込みも含むが、guestのstream読込み／mip-copyは含まない。API間・下層pipeline計測と時間が重なるため合算しない。複数threadのhost時間であってGPU時間や描画thread停止の証明ではない。失敗数はsyscall／HRESULTの失敗で、すべてのWin32やゲーム上の異常を意味しない。既存の5秒reportにだけ出力し、固定atomic配列、各区間の2回の時計読取りを使用する。errnoは時計成功・失敗のどちらでも保持。

**検証。** 実際のpublic shim／bridgeへ計測を加え、計測行だけを除くと元sourceへ全文復元できることを確認。ReadFile／ReadFileEx／WriteFileEx／APC drainの実関数を自作I/Oで動かし、成功・partial・EOF・失敗・64bit offset・共有fd位置・event・遅延callback・errnoの結果を旧新版で比較。3検査は通常／ASan/UBSan双方を通過。変更したclockと新分類を含め、既存histogramの80,000並行記録・stage照合を含む3検査も通過した。これは実iPhoneの速度やI/O並行動作の検証ではない。Macの一括buildにも追加fixtureを含める。新private compileやMacジョブは実行していない。

## 17時台の追加軽量化：音声探索と診断記録の競合

**音声。** 固定public `xaudio2.c`のengine passは、10msを単位として動き、submix段階で毎回64×1,024のvoice slotを調べていた。採用変更はhead／tail／nextの固定配列をstack上に作り、全slotを一度走査してstage別の列を作る。各stageの列を元slot順に辿る。対象選別は1,024回＋実際のsubmix数となり、追加stackはintが4byteのhostで4,608 bytes。voiceの種類／stage／dead／sendの変更とこの走査は既存mutexで保護され、途中の`route`はaccumulation bufferだけを書き換える。callbackは元と同じlock外で実行される。毎pass、そのcallbackの後に列を作るため、直前の変更・破棄を反映する。古いPCM、過去のgraph、処理済みvoiceはcacheしない。

[MicrosoftのCreateSubmixVoice仕様](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2-createsubmixvoice)はProcessingStageの順序を定める。今回の変更ではさらに同stage内の旧slot順も厳密に保ち、浮動小数点加算の順序を変えない。音源数制限・チャンネル削減・resampling品質変更はない。現runtimeの既存音声機能すべてを新たに実装したわけではない。

`test_audio_submix.py`は固定sourceから実際のPCM読取り・補間・route・engine_passを抽出し、著作ゲーム素材を含まない132構成で旧新版を比較する。空slot、全1,024slot、stage境界／範囲外、1〜8ch、明示／既定send matrix、複数submix接続、buffer loop／完了、callbackによる変更・破棄を含む。PCM出力、accumulation buffer、route／callback順、解放後のslot状態をbyte／数値比較。通常とASan/UBSanを通過した。変更部以外のデコーダー・計算式・mutex・callback・cleanupは全文不変も確認。実iPhoneから音を出した検証ではない。

**診断記録。** `dispatch.c`の64slotは各threadのimport呼出しで書き換わり、旧構造はx86_64で48byteのため隣接threadの書込みが同一cache lineに入り得る。structを128byte境界・間隔へ配置し、同じmemberに同じ値を同じ頻度で記録する。外部ABIやguest memoryではなく、このCファイル内のstatic診断領域だけが対象。全sourceを比較するとalignment指定以外に差がない。8threadの実record関数で各field・登録slot・unsigned wrapを比較し、通常／ASan/UBSanを通過。元のWD_MAX超過時の共有slot方式や、watchdog reader側の非atomicな観測方式を修正・検証したとは扱わない。

**局所測定。** WSL x86_64 GCC -O2、自作workload、順番を交替した7ペア。音声の対象選別部分はsubmix 0／1／8／64／256／1,024個の全条件で軽減し、旧約37.9〜62.8μs、新約0.36〜3.21μs。これはrouteを短い自作処理へ置き換えて探索差を測ったもので、mixer全体の速度倍率でもiPhoneのFPS改善率でもない。診断writerは2〜8threadの最初の比較で、全call数当たりwall time中央値が約8.74→2.17ns、7.12→1.98ns、4.87→1.14ns。同時に別の自作sanitizerが走っていたため単独でも確認し、並列条件の短縮は継続したが1threadには大きな揺れが出た。そこで同一許可CPU上の2,000万call×7ペアで確認し、1threadは約4.425→4.392nsとほぼ同等。hostの他作業は制御しておらず、実機のCPU率・温度・FPSの証明にはしない。

原数値は`local/performance-20261113/work-reduction-01/benchmark.json`、`watchdog-benchmark.json`、`watchdog-benchmark-solo.json`、`watchdog-single-pinned.json`。実際の`xaudio2.c`／`dispatch.c`全translation unitも既存publicヘッダーでWSL syntax compileを通過。private guestの再生成は不要。公開の2fixtureは夜の一括Mac buildにも含める。

**採用しなかった案。** 2D／cube／volume textureの連続行をまとめてmemcpyする案も比較した。4,096自作layoutでbyte／padding／slice／alias順序／lock失敗の結果が一致し、ASan/UBSanと実D3DX translation unitのsyntax compileは通った。4KiBや大きなcopyでは短縮したが、256byte×256行＝64KiBでは中央値2,767.6→3,258.2nsと約18％遅かった。iPhoneでの頻度・効果が分からないため、x86に合わせた恣意的な閾値は追加せず、productionの3つのcopy loopを元どおりにした。候補・fixtureは同local directoryの`texture-candidate/`、候補を含む旧stageは`runtime/`に保持。**この旧stageを製品に使わない。** `final-runtime/`は候補を除外した確認stageで、クラウドは最終public adapterから新規stageを作る。

最終adapterの15入力pin・変更2fixture・既存asset計測／mobile overlayを合わせた10検査を通過し、共有CPUヘッダー不変も確認。Mac scriptの構文、公開source検査、diff検査を通過。新Macジョブ・署名・端末更新はこの追加作業では行っていない。

## 19時台：端末能力に合わせるhost処理

[端末適応の調査と実装状況](HARDWARE_ADAPTATION_AUDIT.md)の冒頭に仕様と未完了項目を記載。`device_policy.c/.h`は画質設定を持たない純C policy、`device_platform.m`はAppleの能力取得・notification・QoS・event待機を担当する。起動時の`IOS_DEVICE`、5秒の`IOS_DEVICE_STATE`にCPU数、thermal、低電力、headroom、回収した空poolのbytes、縮小pool数を記録する。headroomは現在の余裕でありRAMの固定上限ではなく、回収量もアプリ全体の常駐RAMが同量減ったという実測ではない。

新しい採用処理は、DXVKの先読みworkerの起動時CPU数とjob境界QoS、driver budgetとアプリ残余を参照した新規pool sizing、完全に空のchunkの回収、FPS追加cap中のevent待機。既存の画像、使用中のresource、guestのCPU ABI／原ゲーム／private guestを変更しない。古いstageを再利用せず、公開adapterから`local/performance-20261113/device-adaptation-01/dxvk`へ新規stageし、`dxvk_memory.cpp`と`dxvk_state_cache.cpp`全体を実依存ヘッダーでsyntax compileした（両方exit0）。

`test_device_memory.py`はallocatorの本物のheader／implementationを省略せず、自作adapterとVulkan driverに接続する。保持中のsliceがある間はpoolが残り、最後のsliceが終了してからだけ解放されること、mapped byteが保持されること、priority／flags別bucket、移動所有権、過大alignment、driver／mapping拒否、32,000並行allocationを確認。`test_device_policy.py`は純C policyの境界と80,000並行更新、および実pacerのevent復帰／中断／epoch変更を確認。4検査は通常とASan/UBSanで合格。追加したApple event待機fixture1件はWSLで明示skipし、一括Mac buildで実行予定。既存の3計測・5操作overlay検査も通過。局所テストは実GPUの完了やiPhoneの改善証明ではない。

低優先度の先読みworkerが同じpipelineの必須描画と競合する時の待ち、回収後の再確保頻度は実機で確認する。メモリ警告中の全走査を100msで抑え、通常は最大の空poolを一つ再利用するため、単に全cacheを毎frame削除する方式にはしていない。`sleep_calls`は実`nanosleep`だけで、Core Foundationのevent待機・OS wake-up全体は含まない。新しい計測を旧Xbox版の数値と合算しない。

## 20時の一括Apple検証と初期化の修正

Codemagic build12（`6af6edd005c8b21ba24ef3d5`、公開commit `08f80fdd0c233c5e60321f28c2f652eb5f2ef8ed`）は7分10秒で終了。自作fixtureは18件成功、Windows専用ARM機械語検査1件skip。Apple専用のevent待機／QoS実行も成功した。Metal Toolchainが最初は無かったが、既定の公式component導入後、3つの自作shaderの実iPhoneOS compileが成功した。依存ライブラリもcompileできたが、アプリ側では`mobile_ui.m:24`の`UIBackgroundTaskInvalid`がCのコンパイル時定数ではないため失敗し、IPAは生成されていない。

修正はstatic変数の値を`skyrim_mobile_initialize`内の`dispatch_once`で設定するもの。SDLのmain threadの入口から呼ばれ、game runtime／overlay作成より前に完了する。再度initializeが呼ばれても進行中のbackground taskを上書きせず、保存workerのticket・取消・終了処理は変更しない。全ログで他のapp compile errorがないことを確認し、無変更での再実行はしていない。回収ZIP SHA256は`421a0f7864628ccd4f97f80b0928c1f7f05b3e1aeef23def6cb8cd667d8a1d13`。原ZIPとbuild.logは`local/codemagic-title12/`。修正版のSDKビルド／署名／実機確認は未完了。

### 修正版build13の成功とbuild4署名

公開commit `951b8d52360277c467c89bd4365ca6d99e91643c`を[Codemagic build13](https://codemagic.io/app/6ad977fcf240708df6941076/build/6af6f17205c8b21ba24ef53c)で一度実行し、4分54秒で成功。18検査成功／既定のWindows専用1件skip、Metal自作3件、iPhoneOS18.0／SDK26.5の本体compile/linkを確認した。新機能を追加して再ビルドすることはしない。公開host SHA256 `aba89d90f41dc93f5a4888de1edc7fa55dd1c1d2f0f951fca97f0b4c7a3f6864`、Metal検証ZIP `9bc44da157f71c53c5acf722ace4b6f1ef3b8ddbd9c4192cb2bb4311aec1df87`、原ログZIP `618caf8fa4bcb8b27aebb379f3907948f0ad0f8b6f6bd0b296ff3928ae440247`。すべて`local/codemagic-title13/`へ回収した。

`x86.h`と`PcGuestABI.h`は新private guestのコンパイル入力に一致。`rt_table.h`は元のconst宣言からhostのlate binding用宣言への既存1行差を厳密に確認し、同じhost宣言がbuild11から不変であることも照合した。構造体・関数ABIの変更はない。元入力と新frameworkの期待hashを照合してPC内で組み立て、unsigned IPA SHA256は`358d72768e8f5114a9c503c5fac7607aeab6ed6385ce3af729110a08f86dc645`。

既存のprofile／DPAPI署名手順により`private/title-signing-20261112/SkyrimIOS-PC2011-build4-signed.ipa`を生成。SHA256は`d32aa66981cc893f4c986c9f473a44c37ee88b238e181b6468b5a91de17c8750`。2イメージ／39,412コードページ／4resourceを暗号検証し、profileのApple chain・既存端末／App ID・メモリ拡張3権限も検証。旧IPA・private guest・ゲーム入力・セーブは保持した。インストールはまだ行っておらず、OS受理・実際のFPS／発熱／音声／描画・cache保存は未確認。

実機更新は本人の保存・完全終了・USB接続・ロック解除後、`snapshot_pc_build4.py`でプロフィールだけを保護し、`phone_pc_build4.py install`でUpgradeする。ゲーム26ファイルは再転送せず、前後の一覧／セーブを照合する。最初は同じsave／同じ画質／同じFPS上限で静止・カメラ・魔法／攻撃を試す。cache比較はその後ホームに戻りしばらく待ってから完全終了し、再起動して同じ条件を使う。新しい計測で初回生成、生成lock待ち、readback／I/O、メモリ回収後の再確保を分け、体感だけから原因や改善倍率を断定しない。
