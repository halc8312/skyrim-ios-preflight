# iPhone diagnostic probe

`ios/SkyrimProbe.xcodeproj`, shared scheme `SkyrimProbe`, builds a native UIKit/Objective-C++ diagnostic app with compiled Metal shaders and a portable C guest-memory probe. It contains **no Skyrim runtime, executable translation, game rendering, game input, or Skyrim save support**. An imported executable is copied only; it is never executed. A successful app build or probe run does not establish Skyrim gameplay feasibility.

## Build and targets

The default target is iPhoneOS ARM64, iPhone only, minimum iOS 18.0. This is an intentionally narrow initial device baseline, not a statement that these APIs require iOS 18. No third-party dependencies or SDK downloads are part of the source build. A Mac with an installed iOS SDK/Xcode is required for Apple compilation.

```sh
xcodebuild -project ios/SkyrimProbe.xcodeproj -scheme SkyrimProbe \
  -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
  -derivedDataPath local/build/ios CODE_SIGNING_ALLOWED=NO build
```

This unsigned build checks compilation/linking only. For a device build, use a locally configured signing team and provisioning profile, and set `PROBE_BUNDLE_ID` to a bundle identifier the team owns (default `org.skyrimios.probe`). Do not commit signing material or device identifiers. `CURRENT_PROJECT_VERSION` and `MARKETING_VERSION` are configurable build settings; preserve issued build records.

The project also allows an ARM64 simulator build, labeled `SIMULATOR (not device evidence)` at runtime. It does not support an Intel simulator by default. The original `SkyrimProbe` scheme retains its app build semantics and empty test action. A separate optional `SkyrimProbeUITests` scheme builds the Objective-C XCTest UI smoke target described below.

## What the app measures

- A visible colored Metal triangle uses precompiled `.metal` functions. Dragging the left half moves its probe state; dragging the right half rotates it. This is geometry/input plumbing, not a Skyrim scene or camera implementation.
- **4 GiB VA** reserves exactly 4,294,967,296 bytes with `PROT_NONE` (`MEM_RESERVE/PAGE_NOACCESS` on Windows). It changes protection on only three OS-sized pages, at guest offsets zero, 2 GiB, and 4 GiB minus one page. Only those pages are touched. The core checks data, guest/host offset roundtrips, uncommitted-page access rejection, and overflow before releasing the reservation. It never requests executable pages, `MAP_JIT`, or JIT entitlements.
- The report distinguishes reserved virtual bytes from touched bytes. On a 16 KiB page device the three-page touch is 48 KiB; on a 4 KiB host it is 12 KiB. Touched bytes are **not** the app's complete physical footprint. Before/after reports separately query `TASK_VM_INFO.phys_footprint` and `os_proc_available_memory`; they are snapshots, not a peak-memory sampler. Metal, UIKit, audio, page tables, and the rest of the app have additional physical costs. Reserve/commit failures remain visible with the failing stage and OS error. A release failure is also explicit.
- **Tone** schedules a 250 ms, 440 Hz tone through AVAudioEngine. The ambient session respects the silent switch. Audibility must be confirmed on the phone. Interruptions/backgrounding stop audio; the next tap explicitly resumes it. Media-service reset recreates the engine.
- **Save/Load** uses bounded, versioned `probe-state.json`, written with Foundation atomic replacement. It persists triangle translation/rotation only and rejects invalid types, nonfinite values, and values outside bounds. It is not a Skyrim save.
- App/build, device model/hardware class, OS, target, thermal state, available memory and footprint are logged. The actual loaded main executable's Mach-O `LC_UUID` is read through dyld with bounded load-command traversal and logged to match a session to its compiled binary/dSYM; unavailable or malformed metadata is explicitly reported. The UUID identifies the executable build, not the user or device. No unique device identifier is collected. Renderer and audio are paused when inactive; probe state is saved on background entry and renderer resumes when active.

## Input copying and evidence

**Import file** and **Import folder** use the Files document picker, security-scoped access, and coordinated reading. The app copies into a new `Application Support/SkyrimProbe/import-<UUID>/payload` and never writes to the selected source. Source descendants are opened relative to their parent descriptors with `O_NOFOLLOW`; symlinks and nonregular/nondirectory objects are rejected. Destination files use exclusive creation. There is no source overwrite, merge into an existing import, or path concatenation from untrusted relative paths.

Copies stream through one 64 KiB heap buffer per import, shared across directory recursion so folder depth does not multiply buffer stack usage. Allocation failure is reported explicitly. Limits are 256 MiB per file, 512 MiB per import, 4,096 descendant entries and depth 16. The operation rejects an oversized file before copying it, and checks actual streamed byte counts if a source changes. These limits intentionally reject a full game installation. The probe is an input-handling diagnostic, not a game-data installer. Copying does not parse or validate the contents for game use.

Each copy starts with a retained `pending.txt`. Only a successful bounded copy with a successful metadata write gets `complete.txt`, which takes precedence over the initial pending record. Failed/incomplete copies remain in their UUID directories without a completion record. Background-time expiry requests cancellation and ends the background task; the next copy loop check stops work. The OS may suspend/terminate the process before that check, so absence of a completion record always means the copy is unusable. There is no automatic deletion of failed input copies or prior logs.

Every launch creates a new persistent session log. **Share logs** exports the current and available previous session via Share Sheet. The visible console is bounded; the on-disk log is preserved. Logging uses error-returning file APIs and synchronizes writes. A small atomic run marker records active, inactive, background, or termination-callback state. An active marker from the previous launch indicates a possible interrupted run; it **cannot distinguish a crash, jetsam, force quit, reboot, or other termination**. No unsafe signal handler attempts to call Foundation after a crash. OS crash/jetsam reports and device build evidence must be collected separately through Xcode/device tools when needed.

## Memory entitlement baseline

No increased-memory-limit or extended-virtual-addressing entitlement is configured. The normal signing entitlements required by a device build are separate from these optional capabilities.

If a real phone fails the 4 GiB reservation, preserve the failing baseline build and log. A future controlled comparison may build the same source with Apple's [Extended Virtual Addressing capability](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.extended-virtual-addressing) enabled and valid provisioning, recording signed entitlements and results for each build. It concerns addressable space; it does not establish a physical-memory budget.

Apple's [increased-memory-limit entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit) applies only on supported devices, and the app must still handle the ordinary limit. Test it separately only if later physical-memory evidence justifies doing so. Apple's [`os_proc_available_memory` documentation](https://developer.apple.com/documentation/os/os_proc_available_memory) says the process limit can change during the app lifetime and is not the phone's total RAM. A sparse reservation success does not imply that 4 GiB can be populated.

Additional implementation references: Apple's [document picker](https://developer.apple.com/documentation/uikit/uidocumentpickerviewcontroller), [audio interruption handling](https://developer.apple.com/documentation/avfaudio/handling-audio-interruptions), and [error-returning file synchronization](https://developer.apple.com/documentation/foundation/filehandle/synchronize()).

## Verification and remaining device work

On the Windows host, MSVC 19.43 x64 builds the portable C core with `/W4 /WX`, and CTest checks bounds, integer overflow, zero-filled sparse pages, high-offset roundtrips, committed-page capacity, and release. The POSIX implementation is also compiled with the preinstalled WSL C compiler using `-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror` and runs the same test. The initial MSVC compile rejected `strcpy` under the warnings-as-errors policy; fixed-size stage formatting was changed to `snprintf`, after which the build passed.

Observed on 2026-11-12: Windows CTest **1/1 passed**; WSL POSIX test **passed**, OS page size 4,096 bytes, three-page probe touch 12,288 bytes. `/usr/bin/time -v` reported 1,576 KiB maximum resident set size for that host test. This is not an iPhone measurement. Info.plist/shared-scheme XML parse and project object references passed a local static check; the current project including the UI-test target has also built successfully in Xcode.

```sh
cmake -S . -B local/build/native
cmake --build local/build/native --config Release
ctest --test-dir local/build/native -C Release --output-on-failure
```

The app at commit `27cafaa7e43b120f9c44030f720d60665bf955de` passed unsigned iPhoneOS ARM64 compilation/linking, product checks and dSYM UUID matching in [GitHub Actions run 43072800930](https://github.com/halc8312/skyrim-ios-preflight/actions/runs/43072800930), with Xcode 26.6/iPhoneOS 26.5. Its separate ARM64 iOS 26.5 Simulator UI test passed in 63.911 seconds (one test, zero failures). Preserved screenshots show the initial Metal triangle and changed movement/yaw restored after process relaunch. The VA text attachment reports `VA PASS`, reservation 4,294,967,296 bytes, page 16,384 bytes, touched 49,152 bytes, released. The separate native macOS ARM64 C test also passed with 16 KiB pages. The raw artifact, exact UUIDs and evidence paths are in [STATUS](STATUS.md). The simulator's available-memory query returned zero; it is not a physical-device RAM measurement.

The earlier successful build and both original CI failures remain retained. **No Apple code executes on this Windows host, and no physical iPhone result is recorded here.** The unsigned device app is not installable. Files-provider import, Share Sheet export, audible sound, real audio interruptions and long-duration behavior still require execution checks. Before any iPhone claim, sign/install the verified device build and record at least:

1. Signed device launch, exact build/OS, visible Metal triangle and working touch state.
2. VA baseline result, page size, touched bytes, and before/after memory values; repeat after background/foreground.
3. Audible tone, silent-switch behavior, interruption recovery, and media reset if reproducible.
4. Probe state surviving save/load and relaunch, plus explicit rejection of corrupt state.
5. A small file and folder copy; no original changes; rejection of oversized, symlink and special-file fixtures; provider/offline/expiry failures. Import code has not been exercised on an Apple Files provider here.
6. Background/foreground and forced termination: renderer resumes, log persists, previous-run marker is qualified correctly, Share Sheet exports readable logs.

Keep signed app/archive/dSYM, compiler logs, failure logs and device results under ignored private paths. None of these checks would by itself demonstrate translated Skyrim code, Windows API compatibility, game loading, or gameplay.

## Optional ARM64 Simulator smoke run

Run `bash scripts/test-probe-simulator.sh` on an ARM64 Mac already configured with Xcode 26.6, an available iOS 26.5 runtime and an existing `iPhone 17 Pro Max` simulator. The script refuses missing/mismatched prerequisites and downloads no runtime, creates no simulator, and changes no signing setup. It boots only the selected simulator, compiles/runs the portable C test as macOS ARM64, then executes the separate `SkyrimProbeUITests` scheme with signing disabled.

The UI test launches the real app, checks viewport presence and Metal pipeline readiness, verifies a real pan changes movement/yaw state, checks save/change/load recovery and persistence across process relaunch, requires a visible VA PASS or explicit VA FAIL result, checks tone scheduling, and exercises Home/background/reactivation. It also validates an actual executable UUID is logged. Screenshots and app-log text are attached to XCTest results. Viewport/pipeline assertions do not perform image-based geometry validation; inspect the captured screenshots. Tone scheduling does not verify audibility. A visible VA failure satisfies the diagnostic UI assertion but does **not** count as a successful memory reservation.

Every run retains a unique `local/simulator/run-<UTC>-<pid>/` with raw runner/compiler/test logs, separate DerivedData, `.xcresult`, exported attachments when supported, a final simulator screenshot, and `scope.json`. The scope records simulator-only status, host/core/UI outcomes and explicit exclusions for device/gameplay/audio proof. A test/build failure keeps evidence and returns a nonzero exit code. Screenshot/attachment export is best effort; screenshots remain inside `.xcresult` even if standalone export fails. The simulator is not erased or deleted afterward.
