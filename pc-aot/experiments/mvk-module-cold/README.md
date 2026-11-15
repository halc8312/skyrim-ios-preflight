# Module-local cold shader generation candidate

The reusable adapter is now `ios/dependencies/patch_mvk_module_cold.py` and is
connected to dependency preparation. `SKYRIM_MVK_MODULE_COLD` defaults **OFF**;
only the exact value `1`, set before renderer creation, selects module cold locks.
The first cold use fixes the choice for the process, so a later environment change
cannot make callers protect the same data with different locks. Restart is required
for comparison. Numeric `IOS_SHADER_POLICY` records `cold_scope=0` (global) or `1`
(module) once per process; its cold-wait metric must be interpreted with that scope.
No new Apple/app build or installed-iPhone update has been performed.

The candidate composes the existing pinned MoltenVK 45887054 hot-cache, library,
and timing adapters, then makes three related changes:

- Retain the old global cold-generation mutex when OFF; select each module's library
  cache mutex when ON. Keep ready lookup, vector publication and serialized-size invalidation
  under the existing catalog lock. Same-module cold variants stay serialized;
  a ready variant of that module still bypasses cold work. No pre-conversion
  `matches()` identity, per-config singleflight, or new worker is introduced.
- Serialize optional debug/file-dump conversion paths with a separate mutex.
  Normal conversion acquires no additional converter mutex: the pinned converter
  holds immutable SPIR-V and constructs `CompilerMSL` per invocation, using the
  caller's private configuration/results. The same module can progress through
  multiple caches or a cache-free route. This diagnostic guard belongs to one
  module object, not its content hash: equal-content objects using different
  caches can still target the same dump filename. A single cache's module cold
  lock provides separate same-key serialization. Diagnostic protection ends before
  library construction and the Metal completion wait. Inputs/results are unchanged.
- Route `MVKMetalCompiler::handleError()` publications whose owner is a pipeline
  cache through that cache's short catalog lock. `MVKConfigurableMixin` uses a
  plain, first-error `VkResult`; making cold compiles concurrent without this
  protection would permit concurrent error-state access. Other owner types keep
  their original setter. No atomic/base-class ABI redesign is introduced.

The error method is called on the requesting thread, after the existing compiler
wait. No new reference to the owner is captured by a late Metal callback. The
fixed source reads cache configuration in `vkCreatePipelineCache` after creation;
`readData` errors also occur during construction, before publication. Cache
destruction still requires callers to finish their API uses. The Metal device
monitor, completion mutex, callback destruction, timeout, cache serialization,
merge implementation, nil-library caching and feedback semantics are retained.

The initial candidate serialized every conversion. Source inspection and the
real-converter experiment below justify narrowing that added mutex to optional
diagnostics. A fixture that enforces exclusion does not prove the original
converter has a race. Debug/file-dump behavior remains conservative; normal
conversion does not inherit an unnecessary wait from the initial candidate.

## Evidence

The default-OFF integration's actual-method suite runs the baseline once and the
same candidate executable with unset, `0`, `1`, and invalid environment settings.
It verifies the policy cannot change after the first cold request, reports exactly
one numeric policy line per candidate process, and repeats ready-hit, same-key,
publication/failure cleanup and error serialization checks in both modes. The normal
suite passed 2 tests in 18.447 seconds; ASan/UBSan passed the same 2 tests in
49.150 seconds and TSan in 21.015 seconds. TSan used the documented test-child-only
ASLR workaround. Apple SDK and real Metal behavior remain unverified. The serialized cache payload and existing callback bodies are unchanged.

`check-metal-alias.sh` now records a separate `mvk-module-cold.log` for this suite.
The hot-cache, timing, library and compressed-copy suites still exercise their
individual baseline adapters, but their final-build comparisons validate every
composed shader file plus the metric header against this adapter. Five focused
host checks against one fresh composed stage passed in 23.264 seconds, including
one-byte drift rejection for each of those six files. The actual build-stage
Git guard accepted first/repeated composition and rejected an unrelated change.
No Mac check script or application build was executed locally.

The product path remains DXVK with flags=0 and no concurrent cache merge. Cache
restore compiles during construction without the catalog lock; external-sync
lookup relies on caller synchronization; existing internal merge only copies
libraries, without compiling under that lock. The short compiler-error publication
lock is retained even in OFF mode, including macro/function errors. Native binary
library errors owned by a shader module still use their original owner path.
These contracts do not authorize arbitrary concurrent Vulkan destruction or merge.

`tests/test_mvk_module_cold.py` compiles the actual pinned/candidate cache lookup,
conversion and compiler error method bodies, plus the real `MVKSmallVector` and
non-atomic configuration mixin. SPIRV-Cross and Metal work are authored boundaries;
this is not execution of Apple APIs or an actual shader conversion benchmark.

- A deliberately paused cold conversion or library construction blocks a cold
  request for an unrelated module in build7, but allows it to finish in the
  candidate before the paused request is released (two instead of one cold work
  item can progress in this two-module fixture).
- The same-module ready hit and a catalog metadata snapshot remain available.
  The snapshot test does not serialize Cereal or validate real driver payloads.
- Eight duplicate callers produce zero extra libraries. Another cold variant of
  that same module remains serialized. Normal cross-cache/cache-free conversion
  progresses concurrently; diagnostic conversion has maximum occupancy one.
  Library wait holds no converter mutex in either mode.
- Injected converter false/exception, library construction exception, publication
  allocation failure, a competing catalog-protected publication, cached nil-library
  behavior, the sequential external-synchronization route, 4,096 ready lookups
  during real vector growth and 16,384 concurrent error publications retain their
  tested contracts. Injected false/exception is an authored boundary, not proof
  that the actual converter returns false on its caught errors.
- `publishReadyForTest` (formerly `mergeReady`) models only a competing publisher
  holding the catalog lock. It does not execute `mergePipelineCaches` or prove
  concurrent flags=0 merging. Vulkan requires caller synchronization for dstCache
  without INTERNALLY_SYNCHRONIZED_MERGE; DXVK does not use merge in this path.
- The revised method fixture passed GCC13 ASan/UBSan (2 tests, 44.285 seconds)
  and TSan (2 tests, 18.721 seconds) on local WSL.

`tests/test_mvk_converter_parallel.py` separately compiles the **real** pinned
MoltenVK converter and SPIRV-Cross, including the existing reviewed sampler-alias
and configuration-match patches. Three authored shader programs with eight
configurations each supply 24 serial reference results. Eight threads perform 256
comparisons with a per-converter mutex and 256 without it: MSL, result logs,
conversion metadata and used-resource flags agree, and input SPIR-V is unchanged.
Both normal GCC13 (45.024 seconds) and full TSan (48.167 seconds) passed. This is
output/concurrency evidence for these inputs, not a throughput or iPhone test.

A later normal run (47.064 seconds) additionally characterizes one existing pinned
converter defect: direct empty authored SPIR-V with an empty configuration logs a
caught MSL conversion error, returns true and leaves MSL empty. This invalid-input
CPU probe does not pass through vkCreateShaderModule validation and does not prove
reachability with valid game shaders. It records an error-contract defect, not a
successful conversion or a production fix. The 512 valid-input comparisons still
passed; the new error case was not rerun under TSan. The renamed method fixture
passed its two normal tests (19.260 seconds); only comments/fixture labels changed
there, so prior sanitizer evidence is reused.

The portable real-converter fixture supplies only platform constants/byte swapping,
uses the upstream `MVK_EXCLUDE_SPIRV_TOOLS` option, and aborts if optional file
export is reached. Actual SPIRV-Tools disassembly, file dumping, Apple SDK behavior
and Metal are outside that fixture. MSL/GLSL result logging is exercised.

The first real-converter TSan run failed at sanitizer startup with an unexpected
memory mapping; it was not a race report or a pass. Successful TSan runs used
`SKYRIM_TSAN_DISABLE_ASLR=1`, which explicitly applies `setarch -R` only to the test
child. System settings and app build flags are unchanged; ordinary test failures
are not retried or suppressed.

Run with public inputs only:

```sh
export SKYRIM_MVK_GPU_SOURCE=/path/to/pinned/MoltenVK/MoltenVK/GPUObjects
CXX=g++ CC=gcc python3 -m unittest discover -s tests -p test_mvk_module_cold.py -v
# Repeat with SKYRIM_TEST_SANITIZERS=1 for ASan/UBSan, or =thread for TSan.
# If WSL TSan cannot start, explicitly set SKYRIM_TSAN_DISABLE_ASLR=1.

export SKYRIM_MVK_CONVERTER_SOURCE=/path/to/pinned/MoltenVKShaderConverter/MoltenVKShaderConverter
export SKYRIM_SPIRV_CROSS_SOURCE=/path/to/patched/pinned/SPIRV-Cross
CXX=g++ python3 -m unittest discover -s tests -p test_mvk_converter_parallel.py -v
# Real converter: normal or SKYRIM_TEST_SANITIZERS=thread.
```

`patch.py <separate-stage> --pristine <pinned-GPUObjects>` only accepts pristine,
reviewed build7-base, or exact candidate files. It validates all inputs before
writing and refuses in-place editing of the pristine directory. This compatibility
entry point delegates to the same dependency adapter used by the build. Applying
it edits only a prepared source stage; it does not run an app build.

## Remaining adoption gates

Apple SDK compilation, actual Metal concurrency/failure/readback, and controlled
iPhone comparisons remain undone. The existing compiler-error race protection is
required even if the first-error value is rarely read; unsynchronized plain writes
are a C++ data race, not merely a reporting concern. Build7's classified histograms
cover MSL and lock waits, not complete role-separated graphics-pipeline duration.
Aggregate role totals do not prove a specific causal chain or distinguish same-key
joining from unrelated-module blocking. A reduction of the redefined cold-lock
metric alone is not an acceptance criterion. More simultaneous library compiles can increase
peak memory, compiler-service contention and heat; bypassing this host mutex does
not prove reduced frame gaps. The mandatory cold compile itself still takes time,
same-module variants still wait, and macro-specialization work is a separate
existing path. Use build7's root/restore/macro and required/prefetch measurements
to decide whether this candidate addresses the observed stalls. Keep the old path
available in the same binary for comparison. No FPS, power or thermal improvement is
claimed from the authored waiting boundaries.
