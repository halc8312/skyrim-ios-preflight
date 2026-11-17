# Graphics pipeline state coordination experiment

This is an isolated host-side prototype. The shipping adapter, iPhone build,
settings, pipeline-cache identity and compute-pipeline path do not import it.
No game shader, asset or translated game code is used by its tests.

The build13 extended capture contains ten required graphics compile waits over
100 ms which end with `CREATE`: after the wait, the requested instance is still
missing. A 1,870.729 ms frame contains 1,808.043 ms of these wait intervals.
That motivates investigating shader-set-wide serialization; it does not prove
this prototype removes those durations on Metal or identify each prior owner's
pipeline state.

## Scope and invariants

The coordinator belongs to one `DxvkGraphicsPipeline`. Its identity is a copied
`DxvkGraphicsPipelineStateInfo`, compared by the unchanged upstream `operator==`,
plus the **exact `DxvkRenderPass*` pointer**. Matching render-pass formats do not
make two different pointers interchangeable. No hash or reduced state key is
introduced.

- A ready lookup retains the original short instance-list lock and returns
  without creating a ticket, reading a clock or entering the priority gate.
- Different missing identities can build independently. Identical requests
  wait for one owner, then recheck the actual published instance.
- Every registration rechecks readiness under the table lock, avoiding a stale
  initial miss after another owner has already completed.
- A ticket is an intrusive stack object. Waiters do not keep its address after
  releasing the table lock. There is one condition variable per coordinator,
  with a predicate checked under the same mutex as removal and notification.
  There is no per-request heap allocation or uncoordinated mutex fallback.
- Required drawing announces itself before table entry. Prefetch admission
  happens before registration and is rechecked under the table lock. No
  registered owner parks behind the required request that depends on it.
- Lock order is table → instance list for rechecks. GPU creation runs outside
  both locks. Publication releases the instance-list lock before ticket cleanup
  obtains the table lock. Published handles are copied under the list lock;
  vector-element pointers cannot escape and become stale after growth.
- Invalid state publishes nothing and has no cache callback. Vulkan failure
  publishes a cached null handle. Exceptions publish nothing and notify waiters
  before propagating. An unpublished non-null Vulkan handle is destroyed if
  vector allocation fails. The counter increments only after publication.
- State-cache callbacks run after ticket cleanup and the required scope ends,
  allowing reentrant work and vector growth. GPU creation, state matching,
  shader conversion, arguments and the existing `CompileScope` remain unchanged.
- Existing worker-join-before-pipeline-destruction remains mandatory. Already
  started compilation is not cancelled. A driver that never completes can still
  prevent shutdown, as with the baseline; a timeout must not manufacture success.

`GRAPHICS_COMPILE_WAIT` in this experiment measures same-identity flight waits,
not the old shader-set-wide mutex wait. Admission sleep is excluded. This change
of meaning must be recorded with any future build; the metric decreasing alone
does not prove improved frame latency. Short table-mutex acquisition is not
individually timed in the prototype.

## Running host checks

With the repository's pinned, prepared DXVK dependency and a native C++17 toolchain:

```sh
python3 -m unittest tests.test_graphics_state_flights
SKYRIM_TEST_SANITIZERS=1 python3 -m unittest tests.test_graphics_state_flights
SKYRIM_TEST_SANITIZERS=thread SKYRIM_TSAN_DISABLE_ASLR=1 \
  python3 -m unittest tests.test_graphics_state_flights
```

The last environment option applies `setarch -R` to the test executable only
on a compatible Linux host; it does not change system-wide settings.

`patch.py` accepts an **already staged, separate copy** from
`ios/dependencies/prepare_dxvk.py`. It validates the pinned upstream fingerprints
and exact adapted input, accepts its own identical output on reapplication, and
rejects changed anchors and source/vendor destinations. The generated header
and implementation are tested together with the real packed-state equality.

## Limits before iPhone adoption

### Apple validation entry point

`tests/test_graphics_state_flights_metal.py` reuses the actual generated
coordination methods, packed state, instance class and pinned destructor with
an authored Metal driver. It compares eight RGBA32Float GPU readbacks between
the baseline and prototype and checks real shader compile rejection, cached null, retry,
publication failure cleanup and balanced fixture-retained PSO references.
Controlled pre-driver gates test admission order, not internal Metal parallelism.

The public host workflow runs this fixture plus the existing native suite under
normal, ASan/UBSan and TSan configurations. A requested fresh
`SKYRIM_GRAPHICS_METAL_OUTPUT` directory retains generated fixture source,
compile/run/platform logs and result JSON, including on failure. The artifact
archive excludes objects and executables. On a Mac, a missing SDK or Metal
device is a failure; only a non-Mac local run skips the GPU case.

Mac run 36 (public commit `ac49eaa`) passed Apple compilation and real GPU
execution on 2026-11-17: all eight authored RGBA readbacks matched in both
models, the intentional shader compile failed once and stayed cached null,
retry succeeded, and retained PSO counts balanced at 9/9 and 10/10. The
prototype alone exercised publication-allocation cleanup. Native normal,
ASan/UBSan and TSan checks also passed. This is not the full DXVK/MoltenVK
rendering path, an iPhone test, or evidence of a performance gain.
The production adapter remains unchanged.

Mac run 34 stopped on Apple Clang diagnostics from the unchanged packed-state
header. Diagnostic exceptions are scoped to that header; authored code retains
`-Werror`. Run 35 then passed the normal and sanitizer native checks, but its
negative Metal linker test was accepted by the driver: a missing interpolant
is not a reliable rejection case. The revised negative uses an authored MSL
`#error`, requires the real Metal API to return nil with NSError, and routes
that failure through the same cached-null publication path. It makes no claim
of testing a PSO linker failure. Original failures and logs are retained.

Verified on 2026-11-17: the normal Linux suite passed all seven tests, including
complete staged `dxvk_graphics.cpp` and `dxvk_pipemanager.cpp` translation-unit
syntax checks. The native baseline/prototype fixture also passed separately
with ASan/UBSan and TSan. Its controlled blocked-owner comparison reported
`different_state_progress=0` for production and `1` for the prototype, while
preserving same-identity single generation. This is an ordering result, not a
latency benchmark. The fixture also checks real packed-state bytes, distinct
render-pass pointers with identical formats, eight concurrent threads, null /
invalid / exception outcomes, publication-allocation failure cleanup, reentrant
callbacks, admission without parked owners, and shutdown admission release.

Portable host tests establish ordering, duplicate suppression, exception
cleanup, publication and native compilation. The separate Mac fixture verifies
authored Metal output; neither establishes full game rendering, compiler-service
scheduling, thermal behavior or actual frame-time gains.
The module-cold and priority switches already shipped remain independent:
MoltenVK may still serialize work even when the DXVK coordinator allows it.

Parallel creation can increase the number of simultaneous Metal requests. Do
not enable this by default on the strength of a shortened host wait. An Apple
SDK build, real Metal output/failure tests, and labelled iPhone comparisons of
frame stalls, required compiler latency, errors, memory and heat are needed.
Cache warming and background time must be separated in those comparisons.

The ordinary build path and its cache namespace are intentionally unchanged
until the prototype is reviewed. No performance improvement on iPhone is
claimed by this experiment.
