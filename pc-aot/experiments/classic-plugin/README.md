# Classic SKSE plugin entry points

This experiment compares a small, authored 32-bit Windows DLL on its native platform and after static translation. Both paths read the same 2,560-byte DLL identified by the digest in `run.py`. The Windows application imports `SKSEPlugin_Query` and `SKSEPlugin_Load` through its ordinary import table. The translated application calls the two functions generated from those exports.

The fixture uses the classic SKSE 1.7.3 entry signatures, 28-byte interface and 12-byte plugin information structure. The sample reads the SKSE and Skyrim runtime versions and editor flag, returns its name and version, and calls `GetReleaseIndex` and `GetPluginHandle`. The reference interface layout and version values are available in the Classic 1.7.3 distribution. [SKSE developer site](https://skse.silverlock.org/)

## Reproduce

Run these commands from `pc-aot`. Use two new output directories under `local/`. Windows LLVM 19.1.1, the x86 MSVC runtime and Windows SDK are used for the reproducible reference.

```powershell
py -3.12 experiments/classic-plugin/run.py windows local/classic-plugin-windows
```

On a POSIX host or WSL, activate the project's translation Python environment with Capstone 5.0.9 and pefile 2024.8.26. Use a local Skydream checkout containing the pinned revision.

```sh
python3 experiments/classic-plugin/run.py compare \
  local/classic-plugin-windows local/classic-plugin-aot \
  --skydream /path/to/skydream --cc clang
```

To compile the translated functions and their harness as arm64 iOS 18.0 device objects, append `--darwin-headers /path/to/public/darwin/include`. The check reads the target CPU, object type, platform and minimum OS from both Mach-O headers.

## What the comparison establishes

The ten cases cover the supported game versions, different SKSE and game versions, an editor launch, an older release index, an invalid plugin handle, two missing callbacks and two missing arguments. The comparison checks both return values, the metadata and name read from the DLL, and the number of calls to each host callback. The translated harness additionally checks the guest stack, return sentinel and callee-saved registers. The experiment only accepts the pinned authored DLL.

The comparator stages the pinned public Skydream source with the project's arithmetic-flag, call-flag and memory-order patches. A one-line adapter lets discovery recognize a PE with no Windows imports. The fixture accesses its two host callbacks through the SKSE interface. An unknown callback stops the test.

The execution evidence covers Windows and the translated desktop harness. The iOS evidence covers compilation. Integration into a signed Classic Lab app, execution on an iPhone, and compatibility with a specific third-party plugin each require their own test. The experiment uses zero game files.

## Serialization across Windows and the translated plugin

A second independently pinned DLL requests SKSE serialization interface 3, version 4. It registers its owner ID and three callbacks. Its save callback writes one four-byte state record. Its load callback iterates records by type, version and length. Its revert callback resets its internal state.

| Runtime | State read | State written |
| --- | ---: | ---: |
| Original Windows DLL | Initial value 11 | 16 |
| Same DLL, statically translated | 16 | 21 |
| Original Windows DLL, fresh process | 21 | 26 |

Each restore begins with different state, exercises the revert callback, then loads the previous process's file. The harness verifies every byte of an unrelated plugin section before and after each write. The translated host uses the project's Classic co-save reader and writer. The Windows host provides an independent implementation restricted to these synthetic files.

On Windows, generate the first save with `py -3.12 experiments/classic-plugin/run_serialization.py windows local/serial-win`. In the same POSIX translation environment as above, run:

```sh
python3 experiments/classic-plugin/run_serialization.py exchange \
  local/serial-win local/serial-aot --skydream /path/to/skydream --cc clang
```

Pass `--darwin-headers` to compile the five translated functions, the function table, the harness and the co-save support for arm64 iOS 18.0. On Windows, complete the round trip with `py -3.12 experiments/classic-plugin/run_serialization.py finish local/serial-win local/serial-aot local/serial-return`.

The test exercises whole-record write, iteration and read. Further tests are needed for partial record operations, form and handle remapping, plugin removal, asynchronous game saves and the real game lifecycle.

## Classic Lab on iPhone

The separately signed Classic Lab target includes the five translated functions from the authored serialization DLL. Its isolated guest memory is allocated when the test runs and released afterward. The same Windows input must produce the same four-byte owner record and preserve the separate opaque record on both platforms.

1. Generate `windows.skse` with the Windows command above. Transfer this 83-byte file to Files on the iPhone.
2. Open the Classic Lab serialization test, select `windows.skse`, and run the test. The expected values are 16 on input, 5 from the first save after Revert, and 21 on output. The result also records one record read and all three registered callbacks.
3. Share both `translated.skse` and `classic-lab-result.json` back to Windows. Run `py -3.12 experiments/classic-plugin/verify_ios_return.py "path/to/translated.skse" "path/to/classic-lab-result.json" --build N`, replacing `N` with the bundle build number of the signed app. This compares the 83 bytes and the hashes, checks the bundle, build, app-reported platform, callback results and run ID, then opens the returned data with the pinned native Windows DLL in a fresh process. Windows must read 21 and save 26. The command creates a new evidence folder under `local/classic-lab-returns` and prints its path. For simulator controls, add `--simulator`. Resharing a previous success retains its original run ID and time.

The phone needs the signed Classic Lab build and the small Windows input. The device or simulator label comes from the app's shared JSON. The test file is deterministic and has the same contents on both. To reproduce the app's three source snapshots from the fixed Windows DLL in the pinned translation environment, run `python3 experiments/classic-plugin/verify_ios_translation.py local/serial-win --skydream /path/to/skydream`. The app-private object target gives the test its own memory and dispatch symbols.
