# Optional Metal 4 library compiler integration

Integration design review, 2026-11-18. The production path is unchanged. No
MoltenVK patch, game conversion or renderer change has been applied. Separate
authored iPhone comparison results inform the readiness gates below.

The smallest useful boundary is MSL source/options to `MTLLibrary`. Function
specialization, legacy pipeline descriptors, PSO creation, encoding, cache format,
and the existing DXVK scheduling remain on their current paths.

## Evidence and pinned baseline

- The built MoltenVK revision is `458870543b9bcf6b0edd6f90aaa776707b310d96`,
  selected by `ios/dependencies/build-dependencies.sh:48,98,181`.
  `source-lock.json:87–103` instead describes the older research-only main-branch
  audit entry; it must not be mistaken for the built MoltenVK pin.
- Pristine `MVKSync.mm` SHA-256 is
  `c36370ad9689a635d88efcd626a872a9b77d8c087b43b886d35c969c58718593`, matching
  `patch_mvk_compile_timing.py:12`. The current composition is
  `patch_mvk_module_cold.py:59–113`, including library ownership and timing patches.
- The 235 existing portable/SDK controls and Darwin TSan lifecycle-model passes
  are reusable evidence, not actual Metal 4 callback or GPU evidence. Run 7,
  source `e0bdd02`, compiled the new archive probe with the macOS and iOS 26.5
  SDKs without warnings; that probe was not executed.
- The available Apple Paravirtual device does not provide the required Metal 4
  capability. The iPhone now supplies the basic library evidence below; the
  device archive and production-lifetime gates still lack execution evidence.
- The separate UIKit comparison host now builds for a physical iPhone and the
  simulator with SDK 26.5 (phone run 4, public `9af993d`, main `3447be4`). Its 56
  relevant controls and simulator unsupported-result/save/share UI checks passed.
  The separate device IPA has since passed private signing verification with
  its dedicated Apple profile and existing certificate. Two physical iPhone
  reports (A19 Pro/iOS 26.5.2, opposite starting backends) passed all 24 normal
  samples, four retained-success GPU cases and twelve intentional rejections.
  These establish the basic ARC library boundary through legacy PSOs, not
  MoltenVK MRC lifetime or a performance improvement. See README for exact times,
  input identities and the unmodified private records.
- Build 2 (`19adfba`) subsequently passed two physical iPhone runs with three
  runtime-input integer workloads and 96 normal samples, all oracle words equal.
  This expands the ARC compatibility evidence. The costly first encounters were
  all legacy-first in run one; the opposite-order run reused the same sources.
  It therefore does not establish a Metal 4 first-use speedup. See README for
  preparation-stage sums, repeated medians and non-compiler elapsed overhead.
- Build 3 (`af678912`) then passed two physical runs with twelve semantic cases
  and 384 normal samples; all 49,152 normal output words and retained controls
  match. Each API leads three cases, but variant cost and the process's first
  request remain confounders. Preparation sums were 740.473878 / 26.471767 ms;
  the other 180 samples after excluding each case's first were 23.753751 /
  24.203348 ms. Snapshot/callback work consumed 86.97% / 95.99% of elapsed time,
  with host storage nested inside callback. This establishes the measurement
  app's overhead and further ARC compatibility, not a backend speedup or the
  unresolved MRC, options and parallel-contention gates.

Upstream source references below are all at the built revision:
[shader implementation](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm),
[compiler lifetime](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKSync.mm),
[device options](https://github.com/KhronosGroup/MoltenVK/blob/458870543b9bcf6b0edd6f90aaa776707b310d96/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm).

## Exact modification boundary

1. `MVKShaderLibraryCompiler::newMTLLibrary`, `MVKShaderModule.mm:559–594`,
   is the dispatch point. Keep `MVKShaderLibrary::compileLibrary:230–248`, its
   retained return contract, and root/restore/macro classification intact.
2. Supply the existing source and `MTLCompileOptions` to a
   `MTL4LibraryDescriptor`, then use the asynchronous Metal 4 library method.
   No archive lookup, new shader generation, or final-PSO API change is needed.
   A synchronous replacement inside the current device monitor would hold that
   monitor for the full compile and invalidate the dispatch timing comparison.
3. Add only the required per-request owner/retirement support around
   `MVKShaderLibraryCompiler` (`MVKShaderModule.h:294–325`). Start with a
   request-owned compiler for validation; measure its factory cost explicitly.
   A shared device-owned compiler is a later optimization requiring extra review.
4. Preserve SDK-26 compile guards and runtime availability checks while keeping
   the app's iOS-18 baseline. Selection should be immutable for the process and
   default OFF. Record requested/selected backend and unsupported reason.
   Only preflight ineligibility may select legacy before a request starts;
   a submitted Metal 4 compile failure must not silently retry through legacy.

`MVKDevice.mm` is deliberately byte-pinned by
`ios/dependencies/verify_lifecycle_baseline.py:10–15` and absent from the allowed
shader edits in `build-dependencies.sh:189–195`. Do not bypass these guards to add
a shared compiler. Any later patch needs an explicit composition/verification
update; the rejected GPU admission changes remain excluded.

## Preserve options and result policy

- Reuse `MVKDevice::getMTLCompileOptions:5217–5269`: effective language version,
  Safe/Relaxed/Fast math selection, Precise/Fast functions, legacy fast-math
  fallback, default optimization level, and position invariance. Do not rebuild
  these from a guessed CLI profile or reset modern options via `fastMathEnabled`.
- Reuse `getMacroValue:597–647` and the dictionary construction at `570–583`.
  Preserve NSNumber float/double and signed/unsigned integer constructors.
  Do not substitute NSString tokens or normalize negative zero as part of a
  backend switch. Existing authored legacy observations do not establish Metal 4
  equivalence, original declaration width from NSNumber getters, or a game bug.
- Keep `handleError:650–657`: a nonnil library plus NSError is a warning;
  nil plus error is failure. Retain both library and NSError through publication.
  `newLibraryWithData` and its separate `handleCompilationError:297–307` are outside
  this first change, as are macro cache publication and its existing failure policy.

## Immediate notification, deferred physical retirement

- Preserve `MVKMetalCompiler::compile:570–590`: dispatch, existing condition-variable
  timeout, and waiter-side error handling. A timeout is not cancellation.
- Reuse the invariant of `owner_retirement.h:31–116`: notify on the first callback
  immediately; defer physical deletion until the launch and notification scopes
  have finished and destruction was requested. Finish launch after task adoption,
  device-monitor exit, and autorelease-pool cleanup, with no later owner access.
- Do not substitute `request_lifetime.h:71–86`: its factory-return barrier delays
  notification when a callback arrives early. [Request lifetime models](README.md#request-lifetime-models) explains why those
  model passes do not make that barrier a drop-in MoltenVK change.
- Production uses manual retain/release (`MVKShaderModule.mm:233–248,594,659–664,709–710`).
  ARC fields in `runtime.mm:196–285` are not equivalent ownership code. Explicitly
  own source, options, descriptor, Metal device/compiler/task, result, and error;
  prove release balance, including early callback and timeout/late completion.
- `MVKDeviceTrackingMixin` only stores a raw Vulkan device pointer
  (`MVKDevice.h:1075–1080`). Retaining the request/compiler does not retain its
  Vulkan owner. Late work must not borrow the caller's vectors/references or
  dereference `_owner`/`getDevice()` after the caller has returned.
- Actual Metal task retirement remains unresolved by the CPU model: do not join
  a task from its callback or while holding completion/catalog/device locks.
  The fixture's unconditional `drain()` is not a bounded production timeout.
  Define and validate the task release/join point without delaying notification.
  In particular, replacing `NoTask` in `mrc_metal.mm` with an actual task is not
  sufficient: the retirement action can run on the callback thread. Transfer
  ownership to a separately validated waiter/retirement context before joining.
  Apple's [task API](https://developer.apple.com/documentation/metal/mtl4compilertask)
  documents a blocking wait; it does not prove callback/release ordering for this
  adapter. The next hardware check should use the existing ARC comparison first.
  The separate `--retirement-handoff-only` CPU model now covers early/late callback
  handoff to a waiter and rejects a join on the producer thread. WSL normal and
  ASan/UBSan controls passed; run12 additionally passed Mac normal, ASan/UBSan
  and Darwin TSan, with empty stderr. It uses fake tasks and does not supply the
  missing Metal 4 wait/release contract, which still requires device validation.

## Failure routing and diagnostics

- Factory nil, task nil, allocation failure, and ObjC/C++ exceptions require
  explicit terminal handling; none may leave the existing waiter pending forever.
  Retain a real NSError when available; otherwise distinguish an adapter error.
  A nil task after an already-delivered callback must never publish twice or
  overwrite the earlier result. Its protocol-fault policy needs a dedicated test.
- Deliver results through the existing `compileComplete/endCompile` path
  (`MVKShaderModule.mm:659–664`, `MVKSync.mm:601–605`). Preserve waiter-side
  `MVKMetalCompiler::handleError`, including the current pipeline-cache owner route
  through `recordShaderCompilationResult` under `_shaderCacheLock`
  (`patch_mvk_module_cold.py:50–56,101–112`). Do not set cache configuration from a
  new callback or bypass this path with `handleCompilationError`.
- Never retain the catalog lock across compilation, task joining, or monitor
  acquisition. The late callback only publishes to/retires its compiler object;
  it does not perform new Vulkan-owner diagnostics.
- Keep `patch_mvk_compile_timing.py:15–32`: dispatch delay, submission-block time,
  and caller's total compile wait remain distinct; post-block observations use
  captured scalars only. Preserve role/source scopes in `patch_mvk_library.py:131–134`.
  Snapshotting inputs or factory initialization must not move cost outside the
  reported scope silently. Report backend/factory cost separately where needed;
  task status, callback receipt, timeout, and actual CPU compile time are not synonyms.

## First three actions and readiness gates

1. Build an isolated MRC adapter fixture using the established retirement model;
   target callback-before-return, timeout/late completion, duplicate callbacks,
   warnings, nil factory/task, exceptions, and owner destruction. Reuse existing
   CPU proofs; add only the actual Objective-C ownership/routing boundary checks.
   The authored `mrc_lifetime.py/.mm` fixture now covers seven Foundation cases
   with separate normal, ASan/UBSan and TSan binaries. Its portable controls pass;
   all seven cases also passed real Mac execution in normal, ASan/UBSan and TSan
   binaries (run8, 2026-11-18). Fake task lifetime and dictionary copies do not
   establish Metal task behavior or `MTLCompileOptions` parity. In run9 the actual
   legacy asynchronous provider passed normal success, invalid-source and controlled
   publication cases, including retained original results/errors and GPU readback.
   Its additional warning case returned a library without the expected NSError;
   that strict failed expectation remains unverified and is now opt-in. Separate
   core normal/ASan/UBSan and concurrent distinct-macro checks all passed in run11
   (public `ad5739b`). Twelve case executions preserved original callback objects
   and single notification/retirement; eight successful GPU cases matched their
   expected words. The two sanitized success cases observed callbacks before
   launch exit. These checks do not cover real Metal 4 task retirement, a whole-
   process leak audit, production options, actual timeout/cancel, or the unobserved
   warning case. Do not add an unexecutable MRC Metal 4 branch just to duplicate
   the existing SDK type/selector evidence.
2. On capable OS-26 hardware, verify real async task retirement and compare both
   library backends through legacy function specialization, PSOs, and exact authored
   GPU readbacks, including existing macro/math/invariance edge cases. SDK compilation
   and virtual-device unsupported reports cannot satisfy this gate.
3. Only after those gates pass, prepare a pinned, default-OFF production adapter
   and an A/B plan preserving options, cache state, role/source timing, failures,
   memory, and thermal observations. No stutter or rendering improvement is established
   until comparable physical-device results exist.

## Practical next device check

On a Metal 4 capable Mac, the existing `runtime.py --async-library` comparison
can exercise the actual task and legacy PSOs. On iPhone, the Python/CLI runner
cannot simply be installed. The new, separately signed UIKit host calls the
same authored comparison after the UI is responsive, retains per-request objects,
and saves JSON in its own sandbox. Its bundle and storage remain separate from
Skyrim, with no game files or save access. Installation and the basic authored
comparison have now succeeded in two opposite-order physical runs.

The iPhone check must record device capability and OS before submission, preserve
unsupported/failure outcomes, and verify the legacy and Metal 4 routes against
the same prescribed GPU output. SDK object compilation alone does not satisfy
this gate. Compare timing only after controlling source/options, request order,
cache history and thermal state; a faster warm run is not proof of a new backend
speedup. Do not turn a blocking task join into a timeout or cancellation claim.

The isolated UIKit host is implemented in `ios/main.mm` with a callable
comparison API and atomic result store. Apple SDK builds, simulator UI review
and dedicated-device signing verification have passed. The build-1 private signed IPA
is 74,013 bytes, with evidence under
`private/metal-probe-signing/run-20261117T233201Z-d9d6ec7f85/`.
Private OTA delivery is verified: page, manifest and full IPA returned 200 over
HTTPS; the delivered hash matched the signed IPA and unauthenticated access
returned 404. The three existing routes were preserved and Funnel remains off.
The user installed the app and supplied two completed physical runs. Both
synchronous and asynchronous timing rankings reverse between runs; a single
small reused source with three samples per route does not establish a speedup.
Build 2 now implements the next serial experiment: three more complex authored
integer workloads with runtime-dependent inputs, fixed expected GPU outputs and
all four routes balanced across execution positions (48 normal samples).
Phone workflow run 5 (`19adfba`) passed 59 controls without skips, both Apple SDK
targets, and six legacy-Metal readbacks on the Apple Paravirtual device. The
dedicated signed build-2 IPA is 85,754 bytes, with evidence under
`private/metal-probe-signing/run-20261118T012351Z-a95e13d8d6/`; the existing private
comparison-app link served that verified package at delivery. Both physical build-2
reports passed all output checks. Their long first-encounter preparation was
concentrated in three legacy-first samples (301.300 / 33.300 / 91.523 ms);
the second run reused the same sources. Repeated timings do not show a consistent
Metal 4 advantage. Whole-run time also contains unmeasured progress/store work.

Before drawing a performance conclusion, the next bounded serial comparison
should use multiple new authored semantic variants with the first requested
route balanced across all four APIs. Keep workload families and variant counts
comparable, preserve fixed CPU oracles and source/options identities, and record
first-observed samples separately. Cosmetic renaming alone is not evidence of a
fresh compiler workload, and no variant scheme proves an empty OS cache. Measure
progress snapshot/store time independently of compiler intervals, preserving
interrupted-result recovery. Build 3 now implements this serial design with
twelve semantic cases and 192 samples, a family-plus-variant leader rotation,
and disjoint snapshot/callback intervals with nested host-store measurements.
Initial/final writes and the return snapshot are explicitly excluded. Portable
and Windows compilation checks passed. Phone run 6 (`af678912`) passed 64 Apple
checks without skips, 24 legacy readbacks on the Apple Paravirtual device, both
iOS build targets and timed Foundation storage. The signed build-3 package is
available through the existing private comparison-app route. Both physical
192-sample runs have since validated outputs and controls, without a demonstrated
Metal 4 speed advantage. Their 587 full-progress checkpoints cost about 6.5 seconds
per run in snapshot generation plus synchronous callbacks, independently of
compiler clocks. Coalesce progress snapshots/storage in the next experiment
without removing per-operation stop checks, final samples, error handling or
interrupted-result recovery. Further serial repeats are not the next acceptance
gate: validate bounded parallel contention, actual Metal 4 MRC task retirement
and production option equivalence independently. The production gate stays
open until those lifetime/options checks pass; neither an ARC fixture nor its
short thermal-zero run proves game performance or long-session stability.

## When Metal 4 could shorten a Skyrim frame

1. **A foreground effect arrives during background compilation.** A 1,487 ms game frame included 1,411 ms waiting for the host's shared cold-generation lock while two prefetch root builds ran. An urgent request can benefit from Metal's scheduling only after the host submits it. Measure the foreground request from submission to callback with and without outstanding prefetch, check whether the intervals really overlap, then compare actual game frame gaps in a separate experiment. [Game trace](../../docs/PERFORMANCE_WORK.md) · [Apple's compilation scheduling](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api) · [Upstream shared-cache report](https://github.com/KhronosGroup/MoltenVK/issues/2863)
2. **Several effects share shader functions and differ in how they blend.** Metal 4 can prepare a common render pipeline and specialize its color format, write mask or blend state when needed. Before adapting the Skyrim renderer, count repeated vertex/fragment pairs with these differences and time final pipeline creation. The earlier 19-minute game trace had 182 library waits over 100 ms and one final pipeline wait over 100 ms. The number of waits includes work outside the foreground frame. [Apple's flexible-pipeline example](https://developer.apple.com/videos/play/wwdc2025/254/)
3. **A known pipeline is available before first use.** Recognizing a spell or particle effect during loading can move eligible work out of the frame in which it appears. This can help even a new player if the necessary input and rendering state are already known. Reusing previously captured pipelines can help later visits. A history of visited areas alone cannot cover an unseen shader. Track loading time as well as first-use frame gaps. [Apple's pipeline archive workflow](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api) · [Godot's first-use examples](https://docs.godotengine.org/en/4.6/tutorials/performance/pipeline_compilations.html)
4. **A familiar crowded scene spends too long preparing GPU commands every frame.** A renderer can reuse command-buffer memory and record independent work on multiple threads. Changing render targets may also need fewer separate passes. Investigate these only if frame traces show significant CPU encoding time or excess passes after the shaders are ready. They require changes to the renderer beyond the compiler used by this probe. [Apple's command encoding examples](https://developer.apple.com/videos/play/wwdc2025/254/)

One Mac player reported 41 to 61 displayed FPS and fewer frame-time spikes in a Seattle scene in The Last of Us Part II under CrossOver/GPTK. Frame generation was in use. Another tester reported improvements in some games and worse frame pacing in others. Both changed the translation stack. Match the scene, settings, cache history and timing boundary before attributing a change to the compiler on iPhone. In Apple's separate iPhone/iPad demonstration, the GPU trace identified excess synchronization between render passes as the cause of a visible hitch. That calls for a different change from scheduling library compilation. [Seattle scene report](https://www.reddit.com/r/macgaming/comments/1v2hgxe/crossover_with_gptk4_metal4_the_last_of_us_ii/) · [Cross-game report](https://www.reddit.com/r/macgaming/comments/1u34g5d/its_seems_like_gptk_4_with_metal_4_is_mature/) · [Apple's iPhone frame analysis](https://developer.apple.com/videos/play/meet-with-apple/242/)
