# Optional Metal 4 library compiler integration

Design review only, 2026-11-18. The production path is unchanged. No applied
MoltenVK patch, game conversion, renderer change, or device run is part of this plan.

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
  capability. A capable Mac/iPhone is still missing for the relevant runtime tests.

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
  notification when a callback arrives early. README:148–170 explains why those
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
   native Mac execution is still pending. Fake task lifetime and dictionary copies
   do not establish Metal task behavior or `MTLCompileOptions` parity.
2. On capable OS-26 hardware, verify real async task retirement and compare both
   library backends through legacy function specialization, PSOs, and exact authored
   GPU readbacks, including existing macro/math/invariance edge cases. SDK compilation
   and virtual-device unsupported reports cannot satisfy this gate.
3. Only after those gates pass, prepare a pinned, default-OFF production adapter
   and an A/B plan preserving options, cache state, role/source timing, failures,
   memory, and thermal observations. No stutter or rendering improvement is established
   until comparable physical-device results exist.
