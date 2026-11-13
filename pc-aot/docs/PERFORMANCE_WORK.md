# PC版の性能改善作業 — 2026-11-13

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
