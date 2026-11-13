# PC版の性能改善作業 — 2026-11-13

**更新 2026-11-14：最新の本人指示が優先。明示的な「go」までアプリ／クラウドビルドは開始しない。** 以下の20時以降の実行予定は11月13日の履歴であり、新しい実行許可ではない。署名済みbuild4は保持。追加作業はPC内の公開コード・自作fixtureの検証のみ。

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
