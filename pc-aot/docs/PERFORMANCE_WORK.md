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

WSL x86_64 GCC -O2でのshadow局所比較（7ペアの中央値、1objectにつき4subresource、実guest変換を別スレッドで実行中）：16objectでは約436→246ns、256objectでは約6,075→245ns、4,096objectでは約181,665→301ns。大量の無関係なmap要素を省けた実験であり、実ゲームでのobject数・呼出頻度・upload量を測った結果ではない。原数値は`local/performance-20261113/runtime/benchmark.json`。

## 棄却・保留した案

- **16バイトSIMD memcpyのbuiltin化は棄却。** 生成513 Cファイルの23,648箇所を見つけたが、実際のDarwinヘッダーではfortified memcpyが既にbuiltinへ展開される。自作7種類のSSE storeで旧／新ARM出力は全文一致。3,328件のnative SSE・全CPU状態・memory比較も一致したが性能改善にならない。実験は`local/performance-20261113/fixed-copy/`。`-fno-builtin`というフラグだけで外部呼出しと判断してはいけない。
- **heapの空bin bitmapは棄却し、productionのheap.cを原文へ戻した。** 同じ割当アドレス・payload・再割当・coalescing・高位bin・失敗・60,000操作／30,000並行alloc/freeとASan/UBSanは通過。一方、空binや上位binでは速くても即時fitで遅くなる。最初の案を2百万組×7ペアで測ると即時fit約2.5%遅く、先頭binを優先する改案でも約15.9%遅かった（並行cross-compileによる雑音を含む）。ゲーム内の分布を測らず総合改善を断定しない。候補の実C・harness・数値は`local/performance-20261113/runtime/`と`runtime-final/`に保持。公開の`test_shadow_ranges.py`はheap不変も確認する。
- **追加memory fence削減は実装しない。** 一部の`ST_FENCE`とrelease storeは局所的には重なるが、DMBは後続の複数アクセスへ、releaseは当該アクセスへ順序を課す。混在するSSE／raw memory経路全体を証明せず削れない。自作MOVUPS/MOVUPD/MOVNTPSでCapstone由来access情報と実store方向の不一致も観測したため、metadataだけで最適化しない。現在の全fenceを保持。

固定アドレスのmemory/string/physics置換は、以前の有限検証が全CPU flags/SIMD/並行処理を覆っていないため、現時点で丸ごと有効化しない。実機でCPU/GPU/シェーダーコンパイルの内訳を測る必要は残る。

## 実行状況

build4として準備中。build3の操作・画質・FPS上限は継承し、画質は自動で変更しない。新しいクラウドビルドは未起動。

- `private/title-performance-20261113-01`でprivate guest再生成完了（378.83秒）。90,390関数・未対応510件は基準と同じ。生成515ファイルと共有ABIヘッダーは基準とbyte単位一致。原入力のハッシュ不変。新helperを含めた514 unitのWindows→iOS ARM64 compile/linkを2並列で実行中（exec session72512）。終了前の結果を成功扱いしない。
- 最終public runtimeの実shadow比較、TSO helper比較、Windows ARM64出力比較を通過。heap棄却後の最終shadow検査（heap原文不変も検証）、WSLのpatch contract 8件、公開source混入checkも通過。host runtime→CPU patch→binding宣言のステージ適用も確認済み。ただし以前の`local/performance-20261113/host-overlay`は棄却heap候補を含む実験stageであり製品に使わない。クラウドは最終public scriptから毎回新規stageを生成する。
- Mac用の新fixture実行ではworkflowの`IPHONEOS_DEPLOYMENT_TARGET`をsubshell内で外し、`SDKROOT`をmacOSに指定。Mac用test binaryへのiOS環境混入を防ぐ。shell構文確認済み。実Macでの新fixtureは一括ビルド時に確認する。
- `Info.plist.in`とassemblerはbuild4を要求。private signing/install/snapshot helperは`private/title-signing-20261112/*_build4.py`として準備済み。旧build3 helper/IPAは保持。署名・端末操作はまだ行っていない。

## 次の作業

1. 進行中private compile/linkを完了確認し、最終heap不変・shadow test・公開ソースcheckを確認。必要なものだけを再実行する。
2. docs/STATUSを更新、rootでcommit。既存公開ミラー`local/prior-preflight/pc-aot/`へ公開sourceだけを反映して同既存branchへpush。Codemagicは手動onlyで、ここでは起動しない。
3. 20:00以降、内蔵ブラウザでCodemagicのFree残時間／他ジョブ／既存commit実行を確認して、`pc2011-title-host`を1回だけ実行。新しい公的fixture2種を同Macで実行する工程を追加している。Mac/Simulatorを実iPhone合格と混同しない。
4. 公開host build4を入手し、共有ABI照合後に新private frameworkとPC内で組み立て、既存profile/DPAPI手順で署名。新private guestに一致する独立manifestが必要で、旧guestのhashを使い回さない。
5. 本人が保存・終了・USB・ロック解除したらprofileのみ最小保護して2/3→4をUpgrade。ゲーム26ファイルは再コピーしない。未接続なら一度だけ準備を案内し、待ち続けて再質問しない。21:00には`skyrim-metal` heartbeatを停止する。

参照：[Clangのmemory builtins](https://clang.llvm.org/docs/LanguageExtensions.html#memory-builtins)。固定サイズのcompiler builtinと通常のCライブラリー呼出しは区別し、実コンパイラーの出力で確認する。

順序の検討：[Armのacquire/release実例](https://learn.arm.com/learning-paths/servers-and-cloud-computing/memory_consistency/examples/)、[LLVM Atomic Guide](https://llvm.org/docs/Atomics.html)。これらは現在の全guest変換の正しさを保証する資料ではない。
