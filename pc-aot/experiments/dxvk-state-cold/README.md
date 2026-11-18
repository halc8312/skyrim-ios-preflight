# Historical graphics-state coordination prototype

This directory retains the historical prototype and its independent serial
oracle. Production now contains both the old serial route and the reviewed
exact-state route, implemented under `ios/dependencies`; it does not import this
directory. The startup-only `SKYRIM_EXACT_GRAPHICS_STATE=1` setting selects the
new route in the same binary. Absent or any other value keeps the default OFF
route, and changing the setting requires a fresh process. No game shader, asset
or translated game code is used by the host fixtures.

The build13 extended capture contains ten required graphics compile waits over
100 ms which end with `CREATE`: after the wait, the requested instance is still
missing. A 1,870.729 ms frame contains 1,808.043 ms of these wait intervals.
That motivates investigating shader-set-wide serialization; it does not prove
this prototype removes those durations on Metal or identify each prior owner's
pipeline state.

## Scope and invariants

These invariants describe the historical prototype and the production ON route.
Production constructs the coordinator only when the startup policy is ON.

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

The historical prototype reused `GRAPHICS_COMPILE_WAIT` for same-identity flight
waits. Current production keeps `GRAPHICS_COMPILE_WAIT` for the serial OFF route
and uses the separate `GRAPHICS_STATE_WAIT` for ON. The startup
`IOS_GRAPHICS_POLICY` record identifies the selected policy and wait scope.
Admission sleep is excluded; a smaller wait metric alone does not prove improved
frame latency. Short table-mutex acquisition is not individually timed.

## Running host checks

With the repository's pinned dependency and a native C++17 toolchain, these
fixtures compile current production once and run separate OFF/ON child processes:

```sh
python3 -m unittest discover -s tests -p test_graphics_state_flights.py -v
SKYRIM_TEST_SANITIZERS=1 python3 -m unittest discover -s tests -p test_graphics_state_flights.py -v
SKYRIM_TEST_SANITIZERS=thread SKYRIM_TSAN_DISABLE_ASLR=1 \
  python3 -m unittest discover -s tests -p test_graphics_state_flights.py -v
```

The last environment option applies `setarch -R` to the test executable only
on a compatible Linux host; it does not change system-wide settings.

Do not apply this directory's `patch.py` to a current production stage.
`prepare_dxvk.stage` now generates both runtime routes, so the historical patch
intentionally rejects that input. It accepts only the old serial adapted text or
its own identical historical output, with pinned upstream fingerprints and
source/vendor destination protection. Current reproduction uses the production
fixtures above and the startup environment setting, without this patch.

## Validation boundaries

### Apple validation entry point

`tests/test_graphics_state_flights_metal.py` reuses the actual generated
coordination methods, packed state, instance class and pinned destructor with
an authored Metal driver. It compares eight RGBA32Float GPU readbacks between
the production OFF and ON routes and checks real shader compile rejection, cached null, retry,
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
Run 36 is predecessor evidence for the prototype, not a pass for the later
production integration. The integrated OFF/ON fixture has passed local normal,
ASan/UBSan and TSan checks and complete graphics/manager source compilation;
its updated real-Metal runner still requires a new Mac execution.

Mac run 34 stopped on Apple Clang diagnostics from the unchanged packed-state
header. Diagnostic exceptions are scoped to that header; authored code retains
`-Werror`. Run 35 then passed the normal and sanitizer native checks, but its
negative Metal linker test was accepted by the driver: a missing interpolant
is not a reliable rejection case. The revised negative uses an authored MSL
`#error`, requires the real Metal API to return nil with NSError, and routes
that failure through the same cached-null publication path. It makes no claim
of testing a PSO linker failure. Original failures and logs are retained.

Historical verification on 2026-11-17: the normal Linux suite passed all seven tests, including
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

The production helper inputs now participate in the build's cache namespace;
OFF/ON in the same binary keeps the same GPU arguments and cache identity.
The setting remains default OFF. No performance improvement on iPhone is
established by the historical prototype or the host integration checks.
