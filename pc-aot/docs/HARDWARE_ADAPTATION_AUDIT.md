# iPhoneの端末性能に応じた実行処理の調査

2026年11月13日。対象はPC版Skyrim 2011の`F:/SkyrimIOS`、調査時点のroot commitは`28e9960820a953e7af6ef5487bddda2abd5d9084`。実機でプレイできているbuild2と、未導入のbuild4候補を区別する。旧Xbox版の診断値は流用しない。

**機種の能力を検出する処理は一部にあるが、ゲーム・描画変換・音声・キャッシュを通じて実行資源を調整する仕組みは不足している。** CPU数やメモリ情報の固定値、Apple向けの優先度対応、D3D9側のメモリ予算管理に改善候補がある。どれが現在のカクつきの主因かは、ソースだけでは確定しない。

目標は、ユーザーが選んだ画質・解像度・FPS上限を保ち、その仕事を各端末で効率よく実行すること。端末名だけで画質プリセットを選ぶ方式にはしない。内部処理の最適化と画質設定を分離する。以下の調査表は実装前の状態を保存したもの。19時台の実装状況を直下へ追記する。

## 19時台にbuild4候補へ追加した対応

- `device_policy`に、安定した端末能力、変化する状態、内部処理方針を分離。Apple APIからCPU数・有効CPU数・画面の最大更新率・GPU family（SDK対応時はApple10まで）・BC対応・統合メモリ・物理RAMを照会する。必要な割り当て時に`os_proc_available_memory()`を取得する。物理RAM、GPU heap、アプリの残余を混同せず、取得した値を画質設定へ書き戻さない。GPU機能の新しい表示は、固定MoltenVKをMetal4対応へ更新したという意味ではない。
- DXVKの先読みcompilerは起動時の有効CPU数を使い、低電力／深刻な熱状態で新規起動する場合は1workerに抑える。通常時の6コア→3workerという既存式と明示的なcompiler数の設定は維持。実行中にworkerを増減させる処理は入れず、job境界でApple QoSを更新する。先読みcompilerはUtility、background／低電力／深刻な熱状態ではBackground、state-cache writerもBackground。QoS APIの失敗は記録し、P/Eコア固定や成功の偽装をしない。ゲームの描画threadや音声threadのQoSは変更していない。
- D3D9の実allocatorは、元のchunk再利用・alignment・priority・flags・resource寿命を維持しつつ、新しいpoolを確保する時だけdriver予算とアプリ残余を参照する。通常は同flags／priorityにつき最大の空chunkを1個残し、余剰の完全な空chunkを解放する。メモリ警告時と予算逼迫時は空chunkを回収する。警告中のallocation側全体走査は100ms間隔を下限とし、毎drawの全走査にはしない。使用中のtexture、live slice、GPU完了前のresourceを削除・縮小しない。
- FPS追加上限の待機は、main threadでCore Foundationのevent待機、workerで残り時間の`nanosleep`へ変更。inputや割り込みの後は元と同じdeadlineを確認し直す。空／停止run loopだけ4ms以下のfallbackを残す。FPS上限、VSync、ゲーム時間、画質選択は変更しない。既存`IOS_TIMING.sleep_calls`には実際の`nanosleep`呼出しだけを記録し、event待機やOS全体のwake-up回数とは扱わない。

動的な割り当て量はadvisoryであり、新しい強制上限ではない。driver予算を超えていても必要なresource自体を黙って捨てず、元のdriver allocation／失敗経路へ進む。最小pool候補は通常4MiBまたは必要サイズで、元のheapに応じた小さいchunk設定も尊重する。メモリ警告は15秒、予算逼迫の回収猶予は2秒。これらはcache保持の方針であり、ゲームの互換メモリ容量の変更ではない。

**ローカル確認：** 実際のallocator全体を自作Vulkan driverと結合し、live resource保持、再利用、priority／flags分離、move後の寿命、mapping失敗、alignment、driver拒否、32,000並行allocationを確認。C policyは境界値・上限超過・警告期限・80,000並行更新を確認。待機関数の呼出側はevent途中復帰・中断・epoch変更・期限超過を自作時計で確認した。通常／ASan/UBSanを通過。変更したDXVKの2つの完全なtranslation unitは実ヘッダーでsyntax compile済み。Appleのrun loop実行fixtureを一括Mac buildへ追加したが、Windows/WSLでは実行していない。UIKit lifecycle、実Metal allocation、FPS・熱・旧端末互換性の実証はまだない。

**残る課題：** Win32 priority／affinityの互換実装、guestのCPU数・メモリ報告値、真の全job共通予算、GPU別の描画経路、120Hz超60FPSの時間進行確認、hotspotの限定AOT置換は別の未完了項目。特に低優先度の先読み中に必須描画が同じpipelineを待つ場合の実機待ち時間を測る。今回のQoS変更だけでカクつきが必ず減るとは判定しない。

**20時のApple側確認：** Codemagic build12で追加の5検査がすべて成功。実CFRunLoopの入力による復帰、workerの期限待機、pthread QoSのUtility／Background切替を確認できた。アプリ本体は`mobile_ui.m`の非定数static initializerで停止したが、初期化をmain threadの起動時へ移した修正版build13はSDK compile/linkも成功した。共有ABIの照合とbuild4のローカル署名も完了。UIKit lifecycle・実Metal cache保存・実機性能の確認は残る。既存profileの3メモリ権限は検証済みだが、実際にアプリへ割り当て可能な量やRAM消費を測ったという意味ではない。

実装時の一次資料：[AppleのQoS](https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon)、[アプリ残余メモリ](https://developer.apple.com/documentation/os/os_proc_available_memory)、[Core Foundationのevent待機](<https://developer.apple.com/documentation/corefoundation/cfrunloopruninmode(_:_:_:)> )。診断には使用率100％や全RAM消費を合格基準として追加しない。

## 現在の対応状況

| 領域 | 現在の動作 | 不足と注意点 |
| --- | --- | --- |
| CPUの検出 | ゲーム側はSDL経由、DXVK側は`std::thread::hardware_concurrency()`経由でhostのCPU数を取得する | ゲームへ返すCPU数は最大4。処理の種類と全体の競合を見た調整はない |
| スレッドの優先度 | ゲームthreadは実pthreadとして実行される | Win32の優先度・affinity指定は成功を返すだけ。native DXVKのcompilerにもApple向けQoS指定がない |
| ゲーム向けメモリ情報 | `GlobalMemoryStatus`は総量1GiB、空き768MiBなどの固定値 | 実際のiPhoneの空き、アプリ上限、guest領域の使用量を反映しない |
| ゲームのアドレス空間 | 32bit guest用に4GiBの仮想領域。内訳に1GiBのheapと2GiBのVirtualAlloc領域がある | host全体のRAM上限とは別。物理RAMの増加だけではguestの32bit制約は変わらない |
| GPU機能 | DXVKとMoltenVKが対応format・機能・各種上限を照会する | 固定MoltenVKのApple GPU family分類はApple9まで。新機種の追加機能をすべて活用している構成ではない |
| GPUメモリ予算 | `VK_EXT_memory_budget`を問い合わせる実装がある | 現行D3D9の通常allocationはこの動的予算を使用せず、UMAでは起動時のheap総量の80％を基準にする |
| 表示とFPS | VSync既定ON。build4候補の追加上限は無制限／24／30／40／60 | ProMotionを含む表示能力と提示間隔を統合した制御はない。追加上限の既定値は0で、常時二重制限とはいえない |
| 温度と電力 | 温度表示と任意の高温時FPS抑制がある | 温度・低電力・メモリ圧力に応じて裏の準備作業量を調整する共通方針はない |

### CPUはゲームへ返す情報とhostの実行を分けて考える

[`kernel32.c`](../vendor/skydream/runtime/kernel32.c)の`guest_cpus()`は`SDL_GetCPUCount()`由来の値を1〜4へ丸め、`GetSystemInfo`と`GetProcessAffinityMask`に使用する。これはゲームがworker数を決める際の入力であり、**アプリ全体を物理4コアへ固定する設定ではない**。DXVK、音声、UIKitなどのhost threadは別に存在する。固定値と優先度の未実装は[以前のソース監査](SOURCE_AUDIT.md)にも関係する既知の互換性課題である。

同ファイルの`SetThreadPriority`、`SetThreadAffinityMask`、`SetProcessAffinityMask`は実際の制御を行わない。今回の最終runtime stageでもこの実装が残ることを確認した。一方、host側のcache保存workerには既に`QOS_CLASS_UTILITY`を指定している。すべてのthreadが無調整という意味ではない。

[`dxvk_state_cache.cpp`](../vendor/skydream/third_party/dxvk/src/dxvk/dxvk_state_cache.cpp)は`((CPU数 - 1) * 5) / 7`を1〜32へ丸めてcompiler worker数を決める。6コアなら3workerで、保存済みのA19 Pro起動ログにも3と記録されている。ただし、この値は実行中のゲームworker数や音声期限を考慮しない。`Lowest`への優先度変更は`#ifndef DXVK_NATIVE`の内側なので、本構成には適用されない。実際の各threadのQoS、CPU滞在時間、待機相手はまだ測っていない。

Appleは、処理の重要度をQoSで伝え、仕事を適切な大きさにまとめ、必要なworkerだけを起こす設計を説明している。P/Eコアへの配置や周波数はOSが負荷・温度などから決める。全threadの最高優先度化、コア数と同数のworkerを各ライブラリで個別に増設する方法は採らない。[Appleのゲーム向けCPU調整](https://developer.apple.com/videos/play/tech-talks/110147/)

SDL 2.32.10のCPU数取得は初回結果を保持し、P/Eコアの役割や進行中の熱状態を分類するものではない。Appleの`activeProcessorCount`も利用可能なCPU数の情報で、各コアの速度を表す値ではない。hostのworker方針には使えるが、起動後のguestへ返すCPU数を頻繁に変える設計は避ける。[SDL API](https://wiki.libsdl.org/SDL2/SDL_GetCPUCount)、[固定SDL実装](https://github.com/libsdl-org/SDL/blob/5d249570393f7a37e037abf22cd6012a4cc56a71/src/cpuinfo/SDL_cpuinfo.c)、[Apple activeProcessorCount](https://developer.apple.com/documentation/foundation/processinfo/activeprocessorcount)

### メモリには異なる三つの制約がある

1. **ゲームへ返す互換APIの値。** `GlobalMemoryStatus`の1GiBは固定の報告値で、実際の現在使用量ではない。`GetAvailableTextureMem`は別系統で、heap総量から計算し、既定では4096MiB未満へ丸める。両者の数字を足して使用可能RAMとみなせない。
2. **guestのアドレス配置。** [`rt.h`](../vendor/skydream/runtime/rt.h)には1GiBのmalloc系heap、2GiBのVirtualAlloc領域、4MiB×30枠のthread stackなどの固定配置がある。報告値の1GiBとは別に、heapには実際のアドレス範囲の制約がある。4GiBの予約全体が直ちに物理RAMを4GiB消費するわけではない。
3. **iOS上のアプリ全体の予算。** guestの常駐ページ、native側の構造、GPU resource、compiler、音声などが同じアプリの使用量へ影響する。GPUのheap総量をアプリ専用RAMと扱えない。

`GlobalMemoryStatus`は32bit fieldの古いAPIなので、12GiBをそのまま書き込む修正は不適切。ゲームの呼出箇所と容量依存の判断を調べ、guestが実際に利用できる範囲と整合した互換値が必要になる。[Microsoft GlobalMemoryStatus](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalmemorystatus)

[`dxvk_adapter.cpp`](../vendor/skydream/third_party/dxvk/src/dxvk/dxvk_adapter.cpp)には動的なheap予算の取得処理があるが、利用箇所を追うとDXGI側の問い合わせと、[`dxvk_memory.cpp`](../vendor/skydream/third_party/dxvk/src/dxvk/dxvk_memory.cpp)のallocation失敗時のログである。本ゲームの通常D3D9 allocationで予算調整に使用する経路はない。同allocatorはUMAのdevice-local heapに対して総量の80％を設定する。これは「実機で80％使っていた」という測定結果ではない。

固定MoltenVKはUMAのheap総量にシステムRAM量を使い、予算にはMetalの`recommendedMaxWorkingSetSize`、利用不能なら利用可能メモリなどを使う。この下層の情報を取得できても、上層が適切に使わなければ全体の管理にはならない。[固定MoltenVKのメモリ実装](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm)

Appleの`os_proc_available_memory()`はアプリが現時点で追加利用できる量の目安で、RAM総量ではない。固定値として保持せず、必要な判断時に更新する。今後はこれとGPU側の予算、アプリ自身の使用量、memory warningを使い、再生成できる未使用cacheや予備allocationを管理する。描画中resourceの破棄やtexture品質の自動低下は行わない。[Appleの利用可能メモリAPI](https://developer.apple.com/documentation/os/os_proc_available_memory)、[メモリ警告](https://developer.apple.com/documentation/uikit/uiapplication/didreceivememorywarningnotification)

### GPUの機能検出は既存処理を活かす

現在の描画経路はD3D9 → 固定DXVK-native → Vulkan → MoltenVK → Metal。CPU側は静的変換したARM64コードであり、描画API変換とは別の経路である。固定版はSkydreamのDXVK 1.9.2表記の派生実装、SDL 2.32.10、MoltenVK 1.4.0。最新DXVKの設定説明をそのまま現行の既定値として扱わない。依存pinは[`build-dependencies.sh`](../ios/dependencies/build-dependencies.sh)にある。

MoltenVKはformat対応、bufferサイズ、threadgroup上限などを実GPUから取得し、対応時にはargument bufferも既定で利用する。Metalを介してA19 ProのGPUを使っているため、GPUコアを一つずつ手動で有効化する処理は必要ない。[固定MoltenVKの設定](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/Docs/MoltenVK_Configuration_Parameters.md)

一方、この版の`getHighestAppleGPU()`はApple9までを列挙する。Appleの表ではA19はApple10であり、過去の実機起動ログもGPU名はA19 Pro、familyはApple9と記録する。**これはA19を旧機種のクロックやコア数に落とす設定ではない。** 新しい機能の認識・利用を見直す候補であり、family番号を変えるだけで速くなる証拠ではない。[固定MoltenVKのfamily判定](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm)、[AppleのGPU機能表](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)

古い端末への対応にはformat互換性も必要。現DXVKはDXT textureをBC1/2/3へ対応させ、Vulkanの対応formatを確認する。AppleのBC対応はGPU世代によって異なり、iOS 18で動く端末すべてについてゲーム全体の互換性を確認済みではない。非対応時に必要な代替処理は画素・mip・sRGB・cube/volumeの意味を保って検証する。単に低画質を選べば解決するという設計にはしない。[D3D9 format対応](../vendor/skydream/third_party/dxvk/src/d3d9/d3d9_format.cpp)、[Appleの対応確認API](https://developer.apple.com/documentation/metal/mtldevice/supportsfamily(_:))

### 表示能力とユーザー設定を分離する

[`mobile_core.c`](../ios/runtime-port/mobile_core.c)の追加FPS上限は0／24／30／40／60、既定0。VSyncは別に既定ON。上限を選んだときは[`ios_runtime.c`](../ios/runtime-port/ios_runtime.c)で最大4msごとにsleepとUIKit pumpを行う。起床回数の削減やVSyncとの整合は候補になるが、入力とmain-threadのイベント処理を止める修正は避ける。

現テンプレートには`CADisableMinimumFrameDurationOnPhone`や表示周期に合わせた範囲指定がない。AppleはProMotionの実際の更新周期をOSが決めること、Core Animationで60Hz超を要求するための設定と`preferredFrameRateRange`を説明している。現アプリのMoltenVK経路が常に60FPSで頭打ちかどうかは、このkeyの有無だけでは断定できない。120FPSのUI選択追加には表示経路の実測とゲームの時間進行・物理の互換性確認も必要になる。[Apple ProMotionガイド](https://developer.apple.com/documentation/quartzcore/optimizing-iphone-and-ipad-apps-to-support-promotion-displays)

`LSSupportsGameMode`はテンプレートではなく[`assemble_title_ipa.py`](../scripts/assemble_title_ipa.py)で既に付与する。温度による30／24への上限制御は、本人が「高温時の自動抑制」を有効にした場合だけ。内部最適化がこの設定を勝手にONにしたり、保存した画質・FPS値を書き換えたりしない。

### 静的再コンパイルの機種別最適化

[`build_title_guest.py`](../scripts/build_title_guest.py)は汎用の`arm64-apple-ios18.0`、`-O1`などの固定条件で生成Cをコンパイルし、特定A19向けの`-mcpu`切替は行わない。host側は別の`-O2`構成。汎用ARM64であってもNEONなどを一切使わないという意味ではなく、コンパイラーや依存ライブラリの実出力を確認する必要がある。

guestへはMMX／SSE／SSE2を備える合成x86 CPUを報告し、RDTSCはmonotonic nsを返す。ここへiPhoneのクロックや未実装のx86命令機能を書き込む変更はしない。機種別のAOT高速経路を作るなら、まず重い小範囲を特定し、元の浮動小数点・overflow・atomic・fence・ABIを保つ実装を汎用経路と比較する。既に棄却したSIMDコピー案や全体O2変更を、機種名だけを根拠に復活させない。

## 第三者の実装から取り入れられる方針

| 資料と実装 | 本件で参考になる点 | 適用範囲 |
| --- | --- | --- |
| [AppleのCPU job調整](https://developer.apple.com/videos/play/tech-talks/110147/) | 適切な仕事の粒度、過剰起床の回避、役割に応じた優先度 | P/Eコアの直接固定や全jobの最高優先度化を提案する資料ではない |
| [Godot WorkerThreadPool](https://docs.godotengine.org/en/4.4/classes/class_workerthreadpool.html) | workerの再利用、taskとgroup task、必要な並列度の選択 | 小さな仕事は並列化で悪化し得る。元ゲームのTLS／callbackを保たずthread poolへ移す根拠にはならない |
| [AMD Vulkan Memory Allocator](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/staying_within_budget.html) | heap総量とdriver予算を区別し、allocation単位と余裕を調整する | 本構成がVMAを使用しているわけではない。allocator全置換を必要条件にしない |
| [Samsung Vulkan Usage Recommendations](https://developer.samsung.com/galaxy-gamedev/resources/articles/usage.html) | 実行時の能力検出、pipeline再利用、resource更新とGPU寿命の管理 | Android向けの値や精度削減をAppleへ無検証で移さない |
| [MoltenVK固定ソース](https://github.com/KhronosGroup/MoltenVK/tree/458870543b9bcf6b0edd6f90aaa776707b310d96) | 現在も使っている機能検出・Metalの能力照会を維持する | 同期submit、fast math、command buffer数などの設定変更には個別の互換性と実測が必要 |

既にbuild4候補へ入れたpipeline cache永続化、準備済みpipeline検索と長い生成lockの分離、音声探索削減などは維持する。[比較記録](PERFORMANCE_WORK.md)を参照。同じ改善を別名で再実装しない。

## 改善へ進む優先順位

| 優先 | 対象 | 実装前に確かめること |
| --- | --- | --- |
| 1 | 能力情報とユーザー設定を別の構造として管理 | CPU数、GPU対応機能、画面能力、メモリ余裕の取得可否と寿命。取得失敗を0性能として扱わない |
| 2 | CPU作業の配分と優先度 | ゲーム・render・compiler・音声のthread数と待ち。Win32優先度の実呼出頻度、handle／TLS／終了寿命を確認してApple QoSへ対応する |
| 3 | CPUとGPUを合わせたメモリ管理 | driver予算とアプリ余裕、guest heap使用量を区別。未使用cacheと予備領域をGPU完了後に回収し、使用中資源と画質を保つ |
| 4 | 表示周期と待機の整理 | 現在のVSync、上限待機、UIKit pump、GPU完了、実present間隔を分離して測る。上限変更はユーザー操作だけにする |
| 5 | 新旧GPUの互換性と機能経路 | 固定MoltenVKとの差分を必要機能単位で評価。古いGPUのBC等の代替、新しいGPUの利用経路を同一描画で比較する |
| 6 | 重いCPU処理の限定AOT置換 | 実際のhotspotを基に既存の部分逆コンパイル計画へ接続。機種別最適化を全ゲームの再構築と混同しない |

内部設計は、起動時の安定した能力情報、必要時に更新するメモリ／温度／電力状態、ユーザーの画質・FPS設定、実行方針の四つに分ける。実行方針が変更できるのは準備作業の同時実行数、cache予算、事前準備の量、再利用やbatchの方法。画質・解像度・描画省略・ゲーム時間の変更権限を持たせない。機能非対応は明示し、別の画質へ黙って切り替えない。

ゲームへ返す最大4 CPUの変更は、この配分方針とは別の互換性変更として扱う。4とhost数の比較には30 thread slot上限、起動時のjob system初期化、他のhost threadとの競合が関係する。増やせば速いとは仮定しない。

## 効果を判定するために残っている情報

既存A19 Proの起動ログはGPU認識、BC対応、memory budget拡張、compiler worker数を確認できるが、各CPUコアの負荷率、GPU処理時間、メモリ帯域の飽和を測ったものではない。build4の`IOS_TIMING`もhost時間の分布であり、GPU時間やCPU/GPUの重なりを直接表さない。

次の実機比較は同じセーブ・場面・画質・FPS上限で行い、初回とcache再利用後を分ける。平均FPSに加え、長いframeの分布、100ms／250ms超の回数、起床・待機、温度の推移、メモリ余裕を見る。既存histogramからは区間単位の推定しかできず、正確なp95/p99が必要な場合は取得方式を明記する。主因の判断にはthread状態とGPU timelineの対応が必要になる。[AppleのMetal性能分析](https://developer.apple.com/documentation/xcode/analyzing-the-performance-of-your-metal-app)

新旧端末の方針は、少なくとも低RAM／60Hz側と新しいProMotion端末で確かめる。自作の能力情報を注入した検査はpolicyやfallbackの確認には使えるが、古い実機の速度・互換性を証明しない。目標は同じ設定での仕事量削減と安定したframe deliveryであり、CPU/GPU使用率100％やRAM全消費を合格条件にしない。
