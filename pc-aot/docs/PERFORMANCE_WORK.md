# PC版の性能改善作業 — 2026-11-13

ユーザーは20〜21時ごろまで実機を接続できない。画質やゲームの結果を維持し、処理量を減らせる変更を比較して採用する。サブエージェントなし。私有ゲームコード・素材・ログ・署名はPC内のみ。旧build2/3と元ゲーム・セーブを保持。Macビルドは20時以降に検証済み変更を一度まとめる。21時以降は新規の長い実験を始めず、実績と残りをまとめて継続設定を停止する。

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
4. 本人が保存・終了・USB・ロック解除したらプロフィールのみ最小保護して2/3→4をUpgrade。ゲーム26ファイルは再コピーしない。未接続なら一度だけ準備を案内し、待ち続けて再質問しない。21:00には`skyrim-metal` heartbeatを停止する。

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

`frame_gap`は成功Present間隔で、backgroundを跨ぐ値は除外。`present_host`はPresent内のhost時間。`pacer`は追加cap処理のhost時間と`nanosleep`呼出数で、UIKit処理時間も含む。残り4種類はHLSL／graphics pipeline／compute pipeline／shader moduleの実API作成処理。pipelineの再利用回数・実cache miss・GPU時間は測っていない。`failures_known=0`の種類について失敗0とは判断しない。atomicの読取境界をまたぐ記録があるため`approximate_window=1`とし、隣接windowを含めて評価する。5秒内の同時増加だけで因果関係を確定しない。

`IOS_PIPE_CACHE event=create offered_bytes=...`はdriverへ渡したbyte数で、命中数／受理済み数ではない。`event=checkpoint stored=1`は今回のファイル保存成功。実機確認は「同一save・同一画質」で初回→ホームへ戻り数秒待つ→完全終了→再起動を比較する。メモリー全体のステート保存やゲームのセーブを置き換える処理ではない。

参照：[Clangのmemory builtins](https://clang.llvm.org/docs/LanguageExtensions.html#memory-builtins)。固定サイズのcompiler builtinと通常のCライブラリー呼出しは区別し、実コンパイラーの出力で確認する。

順序の検討：[Armのacquire/release実例](https://learn.arm.com/learning-paths/servers-and-cloud-computing/memory_consistency/examples/)、[LLVM Atomic Guide](https://llvm.org/docs/Atomics.html)。これらは現在の全guest変換の正しさを保証する資料ではない。
