# Metal compilation experiments

This isolated experiment compares ways to prepare an **authored** shader library.
It is not connected to the application, MoltenVK patches, or shader settings.
The manual `pc2011-metal-compile-checks` workflow runs authored checks only.
The separate `pc2011-metal-legacy-checks` workflow runs archive and option-token
boundaries without requesting a Metal 4 runtime. Neither workflow builds the app.
The tracked experiment contains no Skyrim shaders or game data.

The immediate question is whether a library produced by `MTL4Compiler` or by an
offline Windows compiler can feed the existing Metal function-specialization,
render-pipeline, and compute-pipeline APIs with the same observable results.
That is narrower than migrating the renderer to Metal 4.

## What each environment establishes

`mrc_lifetime.py --output NEW_DIRECTORY` runs a separate Foundation-only ownership
fixture on Mac. Its seven authored cases use manual retain/release, copied Blocks,
an expired caller autorelease pool, immediate/late callbacks, warnings, nil task
returns, and Objective-C exceptions before/after notification. The default runs
normal, ASan/UBSan, and TSan binaries separately. It tracks its owner, fake task,
and token instances; it does not establish Metal task retirement, arbitrary
`MTLCompileOptions` copying, or production behavior. Portable report controls
cannot substitute for those native runs.

`mrc_metal.py --output NEW_DIRECTORY` connects the same owner-retirement model
to the actual legacy asynchronous library API under manual retain/release.
Its success, invalid-source, controlled-publication and warning-source cases
require retained callback objects and exact compute readbacks where applicable.
The warning case specifically requires a usable library plus the original
diagnostic. Normal and ASan/UBSan runs remain separate. Controlled publication
is delayed after actual completion; it does not prove compiler cancellation or
a real compilation timeout. Metal 4 task behavior remains unverified.

The legacy-only CI workflow accepts an optional `authored_windows_library` build
input. It passes this through an environment variable, never shell-source
interpolation. `windows_fixture.py` accepts only one fixed, authored Windows 6.2
product (macOS 15 / MSL 2.4, 11,352 bytes, SHA-256 pinned in the script) and the
matching source hash, then runs the existing three-route GPU comparison. The
binary/base64 stays in private build input and ignored evidence; no generated
library is committed. A matching hash establishes content identity, not a
signature or reconstructed compiler options. Omitting the input leaves this
additional unit unrequested. [Codemagic build inputs](https://docs.codemagic.io/knowledge-codemagic/build-inputs/)
documents the manual per-workflow input mechanism.

| Environment | Can establish | Cannot establish |
| --- | --- | --- |
| Windows with Apple Metal Developer Tools | MSL compilation, diagnostics, generated library size, and offline compiler elapsed time | Apple GPU execution, iPhone frame time, memory pressure, thermal behavior, or runtime Metal 4 speed |
| Mac with SDK and macOS 26 or later and a supported Metal device | Legacy and Metal 4 library creation, existing render/compute pipeline compatibility, GPU readback, and local elapsed time | iPhone gameplay improvement or the full patched DXVK/MoltenVK path |
| Target iPhone | Actual game stalls, startup cost, memory headroom, and thermal behavior | Not exercised by these standalone host scripts |

Apple describes Windows tools as shader/asset build tools. They do not supply a
Windows implementation of the iPhone Metal GPU runtime. An offline compiler's
product version is also not a statement that every Metal 4 runtime feature is
supported.

## Fixed fixture contract

`fixture.metal` supplies vertex, fragment, and compute functions. All routes use
MSL 2.4, disabled fast math, `PROBE_BIAS=3`, and function constant 0 equal to 7.
The render output is an exact RGBA32Float color. Four compute threads return
independently prescribed integer vectors. Runtime comparison also tests invalid
source rejection. A missing compiler, incompatible library, failed GPU command,
or unsupported runtime is not a successful validation result.

The default fixture tests the two library factories synchronously. The optional
`--async-library` route also checks both asynchronous factories, owned source and
option snapshots, retained errors/libraries and Metal 4 task completion. Its
deterministic publication gate tests a timed waiter without releasing the
request. That is not an actual compiler timeout, cancellation or production
MoltenVK lifetime test. Priority propagation and compiler-service contention
under game load remain outside this fixture.

`--probe-library-factory` is a separate opt-in diagnostic, run after the normal
comparison (including an unsupported result). It records the default device,
available device names/families, factory selector, and whether the factory can
create the authored library. It performs no function, PSO or GPU work and never
changes the normal Metal 4 eligibility check or unsupported exit code. A factory
success is not evidence that a virtualized device supports the full Metal 4 API.

## Offline preparation

Install Apple's official Windows tools, then identify the directory containing
`metal.exe` and `metallib.exe`. Use a new output directory for each run:

```powershell
python experiments/metal-compile/offline.py --tools 'C:/path/to/Apple/tools/bin' --target ios --deployment 18.0 --output local/metal-compile/ios-offline-01
```

To prepare the optional library for a **Mac** runtime comparison, generate a
macOS-targeted library instead:

```powershell
python experiments/metal-compile/offline.py --tools 'C:/path/to/Apple/tools/bin' --target macos --deployment 15.0 --output local/metal-compile/mac-offline-01
```

These are authored compiler smoke tests, not a match for every production
MoltenVK compile option. Keep compiler versions, exact arguments, source hashes,
output hashes, and failed-command logs. Do not substitute a host target when an
iPhone compilation fails. Windows elapsed time measures the offline preparation
on that PC; it is not comparable to an iPhone's runtime library latency.

`runtime.py` runs the separate Apple host comparison on a Mac with a sufficiently
new SDK and OS:

```sh
python3 experiments/metal-compile/runtime.py --output local/metal-compile/runtime-01 --repeat 3
```

On the same OS/SDK 26 host with an ordinary Metal device, an explicit
`--legacy-only --library /path/to/macos/fixture.metallib` comparison uses only
legacy source creation and offline library loading. It still requires every
render/compute readback, function-constant result and invalid-source rejection
to match. Optional `--async-library` adds only the legacy async factory and its
controlled success/error retention checks; it creates no Metal 4 task. The
Metal 4 factory probe flag remains incompatible. Results list the exact async
APIs and keep all Metal 4 evidence false. The default
Metal 4 comparison and its unsupported result are unchanged.

It can also receive a macOS-targeted library through
`--library /path/to/mac-offline-01/trial-01/fixture.metallib`. An iOS-targeted
library is not interchangeable with a macOS-targeted library. For the first
Windows run, use `--tools` explicitly after checking the official tool version
and installation layout; automatic discovery does not establish target support.

Raw command logs are written directly to files. On timeout or interruption the
supervisor attempts bounded child/group termination and records unsuccessful
cleanup. A process deadline does not cancel external Apple compiler services.
Both runners accept `--budget` for a shared tool-admission and wait allowance;
process startup, input I/O, cleanup and evidence publication are not a guaranteed
hard wall-clock deadline.

## Effective option observations

`options_probe.py` builds a GPU-free Mac probe of `MTLCompileOptions` getters.
Fresh defaults, legacy and modern math assignments, and typed authored macros
are separate cases. It records availability and observed numeric bits without
claiming to recover the original macro type or production assignment history.
See [option snapshot contract](OPTIONS.md) for the fields and limits.

```sh
python3 experiments/metal-compile/options_probe.py --output local/metal-compile/options-01
```

This captures authored option objects only. It does not compile shaders or prove
that the private batch profile matches effective game options.

## Private generated-source batches

`batch.py prepare` snapshots an explicitly selected local MSL set and a complete
declared profile. `batch.py run` verifies all source hashes/options before
starting a serial compile/link sequence. Both require a new private output
directory. The profile includes target, deployment version, MSL version, math
mode/functions, invariance, optimization and macros; missing values are not
inferred. Source/count/time bounds do not impose a compiler-process RAM cap.

On 2026-11-18, 48 previously generated private inputs (803,016 bytes; 29 distinct
source hashes) compiled and linked successfully with Windows tools 6.2 using
iOS 18.0, MSL 3.1, safe/precise math, invariance false, default optimization and
no macros. All 48 warned about the same unused `boolConstants` variable. Inputs
and complete evidence stay under ignored paths. This is a controlled profile,
not a capture of the game's effective MoltenVK options. No GPU output or iPhone
performance result is established. Duplicate source files were intentionally
retained; byte-identical source alone does not imply identical artifact bytes.

The include checker uses a narrow lexical view that preserves quoted comment
markers and line boundaries, including CR/CRLF and standard line splicing.
Raw strings, trigraphs and nonstandard whitespace splices are explicitly
unsupported; the original compiled bytes remain unchanged. This fixes two
authored cases that the prior comment regex missed. It is not a complete C++
preprocessor or a containment boundary. Location/time macros and `#line` still
make identical source/profile hashes insufficient for semantic reuse, so no
duplicate-compilation skipping is enabled.

## Request lifetime models

`request_lifetime.h` is a separate portable model, not connected to the runtime
fixture or production. It waits for both factory return and callback receipt
before notifying an owner exactly once. That matters when notification can
destroy the owner before a returned task is assigned. The model checks early,
late, simultaneous and defensive duplicate notifications, ownership cycles and
notification failures. When the callback precedes factory return, this barrier
delays notification compared with the fixed MoltenVK behavior. It must not be
adopted as a drop-in production fix.

`owner_retirement.h` is the separate design that preserves immediate completion
notification. Destruction is requested idempotently and deferred until the launch
and callback scopes have finished. It retains the returned task without moving
the existing wait/error semantics into a new completion barrier. This model is
also unconnected to production: actual Objective-C ownership and Metal callback
retirement still need their own tests.

Normal and ASan/UBSan runs passed nine authored cases and 300 concurrent event
iterations for each model. An intentionally unsafe model produced the expected ASan use-after-
free report. This is a model counterexample, not an observed early Metal
callback. The WSL TSan run could not start because of an unexpected memory
mapping; it is not a race-detector pass. Actual Metal task retirement still
needs validation on Apple hardware.

The second Apple-host run passed both models with Darwin TSan, alongside the
normal and ASan/UBSan controls. That closes the model's race-detector gap on this
host, but does not establish actual Metal callback ownership or game behavior.

Five additional host-protocol controls cover a provider that returns nil or
throws before/after a callback, and launch unwinding while a callback is held
open. Normal and ASan/UBSan runs passed with immediate single notification,
preserved first payload, a separate launch-fault record and deferred retirement.
These additions do not establish Objective-C unwinding or actual task release;
their Darwin TSan run is still pending.

The Apple workflow additionally runs both safe models with TSan. The portable
test supervisor bounds the entire unittest worker, permits cooperative child
cleanup before a limited process-group termination fallback, and preserves the
worker log. It does not scan or terminate unrelated processes or prove that
external Apple compiler services have stopped.

## Verification status

On 2026-11-18, the integrated WSL control suite passed 105 tests, with the one
Darwin-only TSan check explicitly skipped. Earlier Windows controls and targeted
POSIX process-control checks also passed. These exercise failure handling,
time limits, retained evidence, expected-output validation, and refusal to promote
synthetic results into Apple GPU evidence. They do not compile the native fixture.
After adding archive/token boundaries and interrupt ownership fixes, the expanded
WSL suite passed 150 of 151 checks; the same Darwin-only check was skipped.
The first expanded run exposed a test-harness heredoc extraction error, which
was corrected; its original failed evidence was preserved. The new aggregate
CI helper keeps independent results and never turns unsupported into success.
With the legacy async route and two-float GPU observation added, the next WSL
suite passed 176 of 177 checks in 31.900 seconds; only Darwin TSan was skipped.
The supervisor also passed a real SIGINT-during-child-creation regression.
Evidence is retained in `local/metal-compile/controls-20261118-08/`.

Apple's signed Metal Developer Tools for Windows 6.2 was subsequently installed
in an ignored project-local directory. Its README identifies Xcode 26.2 as the
matching SDK release; the compiler and linker report version 32023.850. The
installer, launchers, and underlying compiler/linker all passed Authenticode
verification. Their hashes and installation details are retained in
`local/metal-compile/toolchain-6.2.json`.

The real compiler rejected the initial generic `-std=metal2.4` argument. The
runner now uses `ios-metal2.4` or `macos-metal2.4` for that language version;
MSL 3 and later retain the unified spelling. Six related control tests passed,
including 12 target/language combinations. After the correction, three iOS 18.0
trials and three macOS 15.0 trials successfully produced AIR and 11,352-byte
metallib files with no stderr diagnostics. The three artifact hashes match within
each target; the iOS and macOS outputs are distinct. Results are retained in
`local/metal-compile/windows-ios-6.2-02/` and
`local/metal-compile/windows-macos-6.2-01/`.

Four additional authored profiles passed one compile/link trial each with empty
stderr: iOS 18 / MSL 3.2, iOS 26 / MSL 4.0, macOS 15 / MSL 3.2, and macOS 26 /
MSL 4.0. This checks accepted compiler inputs and targets; it does not establish
runtime Metal 4 or production compile-option parity.

The original missing-tool result, rejected-argument result, and unsupported
Windows runtime result remain intact. Successful offline compilation does not
establish Metal 4 runtime compatibility, GPU output correctness, or faster iPhone
gameplay. The first manual Apple-host run at public source `7985e6f` passed 37
control tests, three offline macOS compile/link trials and native Objective-C++
compilation with Xcode 26.6 / SDK 26.5. Runtime returned explicit `unsupported`
because the default device did not pass the Metal 4 family check. The original
report did not record the device name or distinguish a missing device from an
unsupported family. This is not a GPU pass.
The Mac generated its own offline library, so it also does not validate a
Windows-produced artifact on a GPU. Original evidence is retained in
`local/metal-compile/mac-run-01/`. No production default, app build or deployment
was changed.

The second Apple-host run at public source `68a6963` passed all 106 controls,
including Darwin TSan, the actual option-getter probe, offline macOS compilation,
and the adapter's `arm64-apple-ios26.0` object compilation. The only device was
`Apple Paravirtual device`: Mac2 supported, Metal3/Metal4/Apple7/8/9 unsupported.
The explicit Metal 4 compiler factory also returned nil with
`MTLDeviceErrorDomain` code 1. The normal runtime stayed `unsupported` (78);
no Metal 4 library, pipeline, or GPU comparison ran. Evidence remains under
`local/metal-compile/mac-run-02/`. This environment can still test legacy Metal
boundaries, but cannot validate the proposed Metal 4 runtime on an iPhone.

The getter probe preserved all 14 authored numeric macro values, including
negative zero, but getters alone do not expose preprocessor token spelling.
The separate [macro diagnostic probe](MACROS.md) observes that next boundary
through intentional compiler rejection; it does not infer original numeric types.

The third Apple-host run at public source `5539d19` passed all 151 portable
controls (including Darwin TSan), SDK object checks, option getters, offline
compilation and actual macro-token observation. NSNumber float input `0x3f800001`
expanded to `1`, and negative zero to `-0`, while explicit string macros kept
their submitted tokens. This is a compiler-input observation, not yet a GPU or
game-correctness result; see [the macro boundary](MACROS.md).
The archive fixture failed to link because its runner resolved the `clang++`
symlink to `clang`, changing driver mode and omitting automatic C++ runtime
linking. The runner now preserves the reported executable spelling, with a
symlink regression control. The original failed archive evidence is retained in
`local/metal-compile/mac-run-03/`; corrected legacy archive execution was
subsequently verified in run 5.

Run 4 at public source `c86a082` passed all 177 controls, including Darwin TSan.
The two-float macro probe completed actual compute readback and confirmed both
NSNumber/string differences above. Legacy synchronous and asynchronous source
routes also completed matching first-iteration render/compute outputs. However,
offline libraries loaded without exposing the expected functions: the archive
fixture stopped before harvesting, and runtime comparison stopped on its first
offline vertex lookup. Neither full comparison passed. The linker invocation
used resolved `air-lld` rather than `metallib`. Actual symbol inspection found the
entry points in AIR and none in the 92-byte libraries. The runner now preserves
the invocation name and records the resolved binary separately; a symlink
regression covers discovery, version probes and linking. The corrected legacy
comparison and archive execution passed in run 5. Original evidence and both
installed pipeline-script manuals are retained in `local/metal-compile/mac-run-04/`.

Run 5 at public source `fb225fe` passed all 184 controls without a skip. After the
linker fix, legacy synchronous source, asynchronous source and offline-library
routes completed three iterations each with identical 64-word render and
16-word compute readbacks. The offline IR was generated on that Mac with Metal
compiler 32023.883; no Windows-built library was loaded. Controlled async retention
and invalid-source checks also passed. The archive fixture saved one kernel and, in a different process,
strictly found it, strictly rejected an unrecorded kernel, explicitly compiled
the fallback and verified both outputs. Empty/malformed archives were rejected
and saved bytes stayed unchanged. Each native archive process made zero source
library compiles and one IR-library URL load. Evidence is in
`local/metal-compile/mac-run-05/`. These are legacy APIs on the Paravirtual Mac
GPU, not Metal 4 or iPhone gameplay validation. The first and subsequent routes
have different OS-cache histories; their elapsed-time ratio is not a speedup
measurement.

Run 6 at public source `c3c12ae` passed all 214 controls in 18.904 seconds without
a skip, and all six independent units passed. The specialized archive restored
constant 7 strictly, rejected constant 11 for the same kernel name, and produced
the different prescribed output through the explicit value-11 fallback. The
integer macro probe observed `(4294967295, 8, 1)` for NSNumber and the unsuffixed
string, versus `(4294967295, 4, 0)` for the explicit uint string; see
[the three-expression comparison](MACROS.md). The `Apple Paravirtual device`
reported architecture `air64_v27`. Offline IR was generated on that Mac
with Metal compiler 32023.883, not on Windows. Evidence is retained in
`local/metal-compile/mac-run-06/` and [the run log](https://codemagic.io/app/6ad977fcf240708df6941076/build/6afcb807d133a39c4a125e73).
These legacy API observations do not establish Metal 4 runtime support, iPhone
behavior, production parity or a performance improvement.

## Direct Metal 4 archive boundary (runtime pending)

`archive_metal4.py` prepares a narrower future experiment for a macOS 26 host
with a Metal 4 device. It uses the observed `device.architecture.name`, checks
that exact architecture against the installed offline tool's macOS 26 query,
and generates an archive containing only `archive_recorded` from an authored
IR library containing both `archive_recorded` and `archive_unrecorded`.

Each recorded/unrecorded × supplied/nil-library lookup runs in a separate
process. Nil cases reject any library-path argument and never create an
MTLLibrary in application code. All cases call the archive directly; no
MTL4Compiler or compiler fallback is available. Recorded success additionally
requires exact compute readback through an ordinary Metal command encoder.
Unrecorded names must be rejected explicitly. A recorded nil-library rejection
remains a non-success result (exit 3), and an unsupported device remains exit 78.

```sh
# API compilation only; no device query, shader compilation, or GPU execution:
python3 experiments/metal-compile/archive_metal4.py --compile-only --output local/metal-compile/m4-archive-sdk-01
# Future execution requires an actual Metal 4 capable Mac:
python3 experiments/metal-compile/archive_metal4.py --output local/metal-compile/m4-archive-runtime-01
```

The manual Mac workflow includes only the first command. This compiles a Mac
executable and an iPhone SDK object without creating an iPhone application.
The existing cloud GPU is unsupported for the second command; repeating it
cannot supply that missing hardware. Run 7 compiled this probe with Xcode 26.6 /
SDK 26.5 for Mac and iPhone with zero warnings. All 235 controls passed on that
Mac, including Darwin TSan. The compile-only report has no native runs or GPU
work. Portable controls verify process separation, failed command
preservation, fresh output ownership, and report interpretation; they are not
Metal execution evidence.

`library_free_lookup_verified` means no application-created MTLLibrary was
loaded or supplied in that lookup process. It does not establish that the
driver performed no internal compilation or file loading. Descriptors omitted
from the authored script use SDK defaults, so a failed lookup alone does not
identify the cause. This Mac-generated archive also does not validate the
separate Windows-generated iOS archive or production options.

A separate Windows 6.2 authored preparation used this same two-kernel source
and one-recorded-kernel script for macOS 26 / explicit `applegpu_g13g`. It
produced a 10,032-byte Metal 4 archive in a 28.6-second command sequence; AIR
symbol inspection retained both control functions, and archive architecture
inspection reported only g13g. No matching physical device was observed or
used. Evidence is under `local/metal-compile/archive-macos26-windows-01/`;
this establishes offline script/tool acceptance, not runtime lookup.

## Binary archive boundary

The optional `archive.py --specialized` case uses one authored kernel name with
uint function constant 0 set to 7 for harvesting and 11 for an unrecorded state.
It requires the restored value-7 pipeline to hit strictly, value 11 to miss
strictly, and an explicit ordinary value-11 fallback to produce its different
16-word output. Function creation, constant type/index/value, separate process
identity, unchanged saved bytes and invalid archive controls remain checked.
This case requires macOS 15 or newer for device serialization of specialized
functions. Portable controls and actual Apple execution passed in run 6: both
16-word readbacks matched, the saved 5,899-byte archive stayed unchanged, and
the empty/malformed archive controls were rejected.
The existing constant-free fixture and its evidence remain separate.

`archive.py` tests two authored compute kernels through ordinary Metal APIs,
without requiring Metal 4. It compiles a macOS IR library, harvests one kernel
into a device-built archive, then starts another process to reload both files.
The reload must create that PSO with `FailOnBinaryArchiveMiss` and produce the
exact prescribed integers. The unrecorded second kernel must be rejected by the
strict route before an explicit archive-free fallback succeeds. Empty and
malformed archives must be rejected. An arbitrary NSError is not mislabeled as
a specific archive-miss error, and nil device is explicit unsupported.

```sh
python3 experiments/metal-compile/archive.py --output local/metal-compile/archive-new --budget 180
bash scripts/check-metal-compile-mac.sh --legacy-boundaries
```

Each blocking native phase is logged before entering the API. The runner limits
child time, preserves failures, compares process/device identities and archive
hashes, and never calls a source-library factory in the reload process. This
tests archive plus separately supplied IR, not the export of runtime MTLLibrary
bytes. This constant-free boundary passed on the Paravirtual Mac GPU in run 5.

A separate actual Windows Metal 6.2 smoke test produced a 13,680-byte legacy GPU
archive from one authored compute kernel, using iOS 18 and explicit Apple9
targets. `metal-lipo` reported the requested g16p/g16g targets. Its `.mtlp-json`
script and IR are not Metal 4 `.mtl4-json` serialization. The subsequent
`metal-source` extraction emitted an empty file and is retained as an unsupported
extraction result. Neither result validates the Windows archive on a device.

A second Windows smoke test produced a 31,568-byte render archive for the same
iOS 18 / Apple9 targets. The VS/FS entry points and BGRA8Unorm attachment came
from the authored fixture and Apple's documented configuration schema. A local
source snapshot replaced its indexed function constant with the fixed value 7;
the original source hash was unchanged. The result does not validate indexed
function-constant specialization, a Metal 4 archive, or actual GPU execution.
Evidence is in `local/metal-compile/archive-windows-render-ios18-apple9-01/`.
An explicit `-pack-scripts -pack-metallibs` follow-up produced the same archive
hash. Extraction yielded per-architecture library/object files but no pipeline
configuration script, so script extraction remains unsupported for that trial.
Its original inputs were unchanged; evidence is retained in
`local/metal-compile/archive-windows-packed-render-ios18-apple9-01/`. Do not treat
offline archive extraction as a public runtime MTLLibrary export API.

Metal 4's direct archive lookup remains a separate unverified boundary. The
public [`MTL4LibraryFunctionDescriptor.library`](https://developer.apple.com/documentation/metal/mtl4libraryfunctiondescriptor/library)
and function-name descriptions do not establish a `library=nil` render/compute
lookup contract. The explicit unique-name rule on
[`MTL4BinaryFunctionDescriptor.name`](https://developer.apple.com/documentation/metal/mtl4binaryfunctiondescriptor/name)
belongs to binary-function lookup and cannot be transferred to pipeline lookup.
The next capable-device test should compare separate restore processes with a
known IR library, with no application library/compiler factory calls, and with
an unrecorded function name. Call archive factories directly so fallback cannot
hide a miss. If the library-free case passes exact readbacks, then test two
different libraries with the same entry-point names before relying on name
identity. This would establish only the observed OS/GPU/descriptor case, not
absence of internal library work or a universally portable archive key.

The installed Apple `metal4-pipelines-script.5` manual says a missing or empty
script `library` selects the library passed on the `metal-nt` command line.
That offline-script rule does not establish runtime `library=nil` behavior.

`archive_offline.py` reproduces a narrower Windows boundary using the official
`.mtl4-json` schema: one authored, constant-free IR library feeds one compute
and one render descriptor. It fixes iOS 26, Apple9 and `applegpu_g16p`, checks
the target architecture query and each generated archive with `metal-lipo`,
and preserves sources, tools, scripts, hashes and failures. Commands run one
at a time with a shared 180-second admission/wait budget; no compiler memory
ceiling or hard bound on external services is claimed. It accepts no arbitrary
shader or configuration input, and it is not part of the iPhone app.

```powershell
python experiments/metal-compile/archive_offline.py --tools F:/SkyrimIOS/local/tools/metal-6.2/bin --output local/metal-compile/archive-offline-new
```

The 13 portable controls passed, followed by one real Windows Metal 6.2 run:
all 12 commands succeeded, with compute/render archives of 12,704/15,600 bytes,
both reporting only `applegpu_g16p`. Evidence is in
`local/metal-compile/archive-offline-runner-windows-01/`. This establishes
offline Metal 4 script translation only. GPU loading/readback, library-free
lookup, game settings equivalence and iPhone performance remain unverified.

The optional `--specialized` mode uses one authored function and uint constant
index 0, generates value-7/value-11 archives separately, and checks omission of
the required value. Its 22 portable controls passed. The real Windows 6.2 run
generated distinct 12,368/12,384-byte positive archives, but the omitted-value
case returned exit 0 with a warning and an archive. The runner therefore reports
`failed`, retains both positive results and the negative evidence, and does not
claim the compiler enforces this boundary. This is not a GPU or Skyrim failure.
Evidence: `local/metal-compile/archive-specialized-runner-windows-01/`.
No warning-promotion option was added. Before replaying captured recipes, required
constant completeness needs separate validation; archive existence is insufficient.

```powershell
python experiments/metal-compile/archive_offline.py --specialized --tools F:/SkyrimIOS/local/tools/metal-6.2/bin --output local/metal-compile/archive-specialized-new
```

Family filtering is not an exclusive chip selection: this tool's iOS 26 Apple9
query includes six architectures, including both returned for Apple10. The
fixed `g16p` artifact must not be assumed to match either test phone. Apple's
feature tables identify A17 Pro as Apple9 and A19 Pro as Apple10, but family
support alone does not establish the artifact architecture. Record the actual
[`device.architecture.name`](https://developer.apple.com/documentation/metal/mtlarchitecture/name),
OS and runtime capabilities, then verify loading and matching GPU outputs on
that device. The isolated runtime fixture now records the architecture name;
the production phone diagnostics are unchanged.

```sh
python -m unittest discover -s tests -p 'test_metal_compile_*.py' -v
```

## Before integrating with MoltenVK

The [pinned integration plan](MOLTENVK_PLAN.md) identifies the asynchronous
library boundary, manual ownership requirements, and the remaining runtime
gates. It is a reviewed design, not an applied production patch.
The separate [direct archive plan](ARCHIVE_PLAN.md) explains the earlier lookup
and conversion-metadata work needed to avoid library creation, including the
specialization and eager-cache-restoration limits.

1. Establish actual Apple compilation and matching render/compute readbacks for
   the authored source through the chosen API route. A later representative-MSL
   comparison also needs exact production options. Additional game-data conversion
   was rejected by automatic approval review and remains unperformed; current work
   uses authored inputs or reads existing outputs. Synthetic colors are not game
   fidelity.
2. Preserve the existing source, macros, language version, fast-math settings,
   failures, ownership, and callback lifetime. Do not put a synchronous Metal 4
   compile under the existing device monitor: that would hold the monitor for
   the entire compilation, unlike the current asynchronous launch.
3. Capture request origin before dispatch. The current timing patch captures
   context as values; it does not restore thread-local context inside a reused
   GCD worker. Replacing the API alone does not separate draw and prefetch QoS.
4. Keep started work alive through completion. A host-side timeout is not Metal
   task cancellation. Validate this before adding the new route to the app.
5. Compare one change at a time on the target phone. Preserve the working path
   for unsupported OS/device combinations and evaluate actual frame gaps,
   startup time, failures, memory, and temperature.

## Connection to first-use preparation

The three external reviews' common-recipe proposal still needs actual shader
inputs. The bounded private inventory in `experiments/shader-inputs/` identified
10,475 candidate records across 122 FXP entries, with 6,659 distinct stage/hash
keys (309 VS and 6,350 FS). An authored-only pinned DXSO oracle agrees on 21
synthetic consumption/stage/hash cases under ASan/UBSan. This is not complete FXP
grammar validation or measured runtime pipeline coverage. A subsequent bounded
native check matched consumption/stage/hash for 32 real candidates (16 VS and
16 PS, 61,392 bytes), after 28 authored boundary/limit controls passed. These
children used guard pages, UBSan and OS limits; no SPIR-V compilation or GPU work
was performed. The actual device `.dxvk-cache` has not been located in the
available local files; current numeric diagnostics do not contain that cache.

The next input study would compare existing candidate records and DXVK stage/hash
keys with a private device-cache copy once available. Any new game-data conversion
still needs resolution of the approval-review block above. Generated
fixed-function shaders and unresolved keys remain separate categories. Only then
can a small, known recipe set test early library or final-pipeline preparation.
History-based weights can refine that set later; they cannot supply missing
shader inputs for a first-time player.

## Primary references

- [Apple Metal tools](https://developer.apple.com/metal/tools/)
- [Metal 4 compilation workflow](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api)
- [MTL4LibraryDescriptor](https://developer.apple.com/documentation/metal/mtl4librarydescriptor)
- [MTL4Compiler](https://developer.apple.com/documentation/metal/mtl4compiler)
- [Binary archive harvesting](https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects)
