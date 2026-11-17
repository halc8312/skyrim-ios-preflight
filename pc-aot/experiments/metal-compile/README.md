# Metal compilation experiments

This isolated experiment compares ways to prepare an **authored** shader library.
It is not connected to the application, MoltenVK patches, shader settings, or CI.
It contains no Skyrim shaders or game data.

The immediate question is whether a library produced by `MTL4Compiler` or by an
offline Windows compiler can feed the existing Metal function-specialization,
render-pipeline, and compute-pipeline APIs with the same observable results.
That is narrower than migrating the renderer to Metal 4.

## What each environment establishes

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

The fixture tests the two library factories synchronously to keep their scope
comparable. It does not test production asynchronous callbacks, timeouts,
priority propagation, or the Metal compiler service under game load.

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

It can also receive a macOS-targeted library through
`--library /path/to/mac-offline-01/trial-01/fixture.metallib`. An iOS-targeted
library is not interchangeable with a macOS-targeted library. For the first
Windows run, use `--tools` explicitly after checking the official tool version
and installation layout; automatic discovery does not establish target support.

Raw command logs are written directly to files. On timeout or interruption the
supervisor attempts bounded child/group termination and records unsuccessful
cleanup. A process deadline does not cancel external Apple compiler services.

## Verification status

On 2026-11-18, the combined Windows control suite passed 36 tests. Three targeted
POSIX process-control checks also passed in WSL. These exercise failure handling,
time limits, retained evidence, expected-output validation, and refusal to promote
synthetic results into Apple GPU evidence. They do not compile the native fixture.

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

The original missing-tool result, rejected-argument result, and unsupported
Windows runtime result remain intact. Successful offline compilation does not
establish Metal 4 runtime compatibility, GPU output correctness, or faster iPhone
gameplay. The native comparison and SDK/release alignment remain to be validated
on Apple hardware. No production default, CI workflow, app build, or deployment
was changed.

```sh
python -m unittest discover -s tests -p 'test_metal_compile_*.py' -v
```

## Before integrating with MoltenVK

1. Establish actual Apple compilation and matching render/compute readbacks for
   the authored source. Then repeat with privately held representative generated
   MSL and its exact production options. Synthetic colors are not game fidelity.
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
inputs. A private prior inventory contains 10,475 SM3 candidates across 122 FXP
entries, including 48 previously translated cases. This is not measured coverage
of the runtime pipelines. The old extractor needs bounded decompression and
token/record validation before reuse, and the actual device `.dxvk-cache` has not
yet been located in the available local files. Current numeric diagnostics do
not contain that cache.

The next input study therefore classifies and validates candidates, reconstructs
DXVK stage/hash keys, and compares them with a private device-cache copy. Generated
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
