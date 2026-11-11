# Experimental owned guest host

Build9 adds local graphics/audio preferences, seven presets and a settings hub
available before guest startup as well as while running. Original settings are
the default. Console video-mode requests (360p/540p/720p/1080p), integer EDRAM
supersampling (1/2/3, bounded to3840x2160), mirror/cache budgets freeze at renderer
attachment. Only observed source sizes establish whether a game honored its
video mode; a requested mode is not a verified internal render resolution.
Presentation-only scaling/filter/sharpening/FPS limits are atomic live tuples.
Admission preserves average selected cadence at guest swaps, including noninteger
rate ratios, and discards missed slots after a stall without catch-up bursts.
It does not enforce an absolute minimum interval or synchronize to screen scanout.
These controls do not skip scene draws or unlock guest simulation rates. Display transfer
and newly decoded frame rates are measured separately from actual screen scanout.

An optional host audio observer feeds bounded per-client48kHz stereo queues.
The native AVAudioSourceNode/AVAudioEngine adapts these to the active output;
there is no microphone or background-audio entitlement. Route disconnection and
media reset require explicit resume. Background suspension also stops output.
Gain/mute/profile controls are live; reported reservoir depth is not guaranteed
end-to-end latency. Starting this output transport does not prove that the
separate XMA decoder/MMIO path delivered game audio. Numeric receive counters
keep that limitation visible. Public fixtures and simulator tests never start
an owned title or establish physical-iPhone performance/audio compatibility.

This independent host foundation contains no game code, game assets, keys,
`guest-sys` dependency or generated private layout. It does not modify the
frozen resident fixture, its cloud workflow, or the upstream workspace.

Default Rust tests use self-authored metadata. `apple-host` is checked only for
plain ARM64 iOS/device or Simulator. A Rust target check does not compile the
Objective-C++ SDK helpers, link an app, authenticate a signature, load an image,
execute a game, or establish iPhone compatibility. The separately tested public
resident fixture remains a different experiment.

The current local checks passed 20 synthetic Rust tests (19 portable policy/
boundary tests and one fixed public-XEX test), and the complete
`metal-renderer` Rust graph passed `aarch64-apple-ios` type checking. The
separate resident experiment has an independently checked successful Simulator
case for its direct native jump and real Kernel ThreadExit/RAII path. Reusing
those sources does not turn the new host's still-pending SDK gate into a pass.

The future signed app supplies the following separately prepared expectations:

- `OwnedGuestBinding.json` at the main bundle root, with exactly the 27 fields
  defined by `binding.rs`, including a separately trusted original unsigned
  full SHA, the explicit candidate normalization manifest, expected XEX SHA and
  identity, image/code configuration, function count and LE-u32 table SHA, and
  all 20 selected PPCContext layout words.
- `Frameworks/PrivateGuest.framework/PrivateGuest`, a thin signed ARM64 image.
- A user-selected directory within the app sandbox containing the expected
  plaintext `default.xex` and original game files. The same owned XEX buffer is
  hashed before parsing. No decryption or game download occurs here.
- A new, disjoint diagnostic directory within that sandbox. Existing diagnostic
  directories are refused. Game VFS mounts remain read-only.
- `Documents/OwnedSaves`, a real sandbox directory excluded from backup by the
  app before preparation. The verified game identity and XEX SHA select a save
  namespace. A new exclusive session receives a validated copy of the preceding
  session's title content; prior sessions are preserved. The process retains its
  store lock for the entire guest lifetime. Diagnostics are never seeded as saves.

The signed-image candidate verifier reconstructs exactly the permitted 40
original byte positions, truncates the appended signature, checks the external
unsigned full SHA, and derives the candidate manifest again. Its narrow LLD
zero-padding rule is explicit. Neither the binding nor signature envelope
authenticates an Apple signer, entitlements, AltServer behavior, or OS loading
permission. Those remain external gates. The expectation resource must come
from the trusted packaging process; reading a hash from the observed framework
would defeat this binding.

Preparation is one-shot and synchronous on a worker. It pins regular files,
refuses symlink components, validates the XEX/import preflight, validates the
framework content/mapping/platform/typed seven APIs, copies the bounded readonly
function table, creates Runtime and Kernel, installs the global Kernel, and
binds the live atomic EA/dirty arrays. The SDK returns the 49-word actual C
layout and opaque jump-buffer contract. No ordinary function wraps setjmp;
native provider pointers remain direct. Kernel call and guest call are
`C-unwind`; their guest worker boundary catches ThreadExit. No Swift frame may
participate in guest exception or longjmp flow. Guest caller jump-buffer
capacity/alignment and complete imported behavior remain separate proof duties.

The loader retains its file descriptor and dylib handle for process lifetime,
uses SDK `RTLD_FIRST`, verifies symbol dyld image ownership, compares mapped
header and RX bytes, and rechecks the retained file identity and complete hash.
It refuses image initializer/TLS/interpose sections and requires the expected
ordinary libSystem dependency. These checks detect ordinary replacement races;
path-based dlopen still is not pre-execution inode attestation. A separate native
provider image is not proof of OS authenticity against an actively compromised
process. Library constructors from arbitrary untrusted images are unsupported.

`OHPrepareV1` returns an opaque numeric token. The trusted Rust renderer uses
`prepared_kernel(token)` then `mark_renderer_attached(token)`, retaining its
renderer and UIKit CAMetalLayer for process lifetime. `OHStartV1` refuses before
that step and, with `metal-renderer`, checks live renderer readiness again.
The native iOS attachment selects upstream's handheld GPU-effect synchronization
default before starting the renderer. Fence/interrupt/report effects wait for
renderer queue progress; this does not assert completion of every Metal command
or establish that New Game crashes are fixed. Desktop defaults are unchanged.
Only ordinary controller state enters through `OHUpdatePadV1`; raw guest
contexts, arguments and addresses are not part of the Swift API.

An allocation preflight additionally requires an explicit plaintext,
uncompressed XEX format record, a bounded optional-header table and security
block, and payload length exactly equal to the separately expected image size.
Implicit raw, compressed, truncated, trailing-byte and duplicate-header forms
are intentionally refused rather than silently zero-expanded by the upstream
parser. This is a new-host staging restriction, not a change to the upstream
XEX parser or earlier PC artifacts.

StrictAll is the default and refuses every missing function or variable before
loading a guest image. Explicit policy 1 is diagnostic CpuDiagnosticTrap: missing
variables still refuse, and the first unknown function writes a numeric failure
record and terminates the process, without a successful zero stub. It does not
make the real game's currently missing imports supported. UI-facing readiness,
signature authentication and iPhone game verification remain false.

Diagnosis lasts 1 through 600 seconds. An independent worker enforces process
`_exit` at that ceiling, including after main-thread exit since secondary guest
threads may survive. Another worker attempts a complete numeric JSON record one
second earlier. File sync then exclusive hard-link publication prevents a killed
writer exposing partial final JSON; power-loss durability is not asserted.
Unknown/fatal paths have their own one-second forced-exit ceiling so evidence I/O
cannot hold the failed process alive. There is no reset or second start.

Normal play uses the separate `OHPlayV1` entry point. It retains the same
preparation, renderer, content binding, one-shot and fatal-error checks, but
creates no process deadline or timed diagnostic writers. One condition-variable
waiter records the kernel exit notification without polling. An exit notification
does not stop surviving guest workers. Existing identity-bound `OwnedSaves`
sessions remain retained for the process lifetime and are resumed on the next
launch; removing the diagnostic timer does not establish game-save compatibility
or long-session stability. iOS lifecycle and audio handling are unchanged.

`native/OwnedHost.h` is the UI C ABI. Status is 96 bytes. State values are
0 fresh, 1 preparing, 2 prepared, 3 renderer attached, 4 running, 5 kernel exit notified,
6 failed. Preparation failure categories are 101 paths, 102 binding, 103 XEX,
104 imports, 105 SDK layout, 106 image loading, 107 descriptor, 108 Runtime,
109 Kernel, 110 host binding. Category 24 is first missing import; 20–23 are
adapter failures; 25 is start failure. These are diagnostic categories, not
guest error return values. No user-visible API returns game bytes or image data.

Numeric JSON diagnostic schema 2 separates a guest fatal category from kernel
module/ordinal identity. Failure 22 now leaves those HLE fields zero and records
`guest_fatal_category`; category 3 additionally preserves only a u32 dispatch
target and metadata flags (alignment, code range, exact registered membership,
zero, or rejected high detail). No other callback detail is saved. This is
evidence for diagnosing a missing target, not a dispatch fallback or crash fix.
The C Status layout and owned guest ABI remain unchanged.

For a later SDK build, compile this single Rust staticlib with
`--locked --features metal-renderer --target aarch64-apple-ios` (or
`aarch64-apple-ios-sim`), supplying the matching official-source
`WGPU_NATIVE_FRAMEWORK_DIR`. Compile `native/OwnedHostNative.mm` with the official
Apple SDK and the public `PrivateGuestABI.h`; include `native/OwnedHost.h` in the
app bridging header. Embed/sign the matching dynamic WgpuNative framework.
Do not combine a second runtime staticlib into this app or copy an Apple SDK to
Windows. Actual SDK linking, Simulator execution, real signing/re-signing,
private guest loading, gameplay and iPhone execution have not been validated
for this new host.

For a public-only SDK fixture gate, `native/PublicOwnedFixture.cpp` is a copy
of the already SDK-tested three-entry fixture, using the unchanged generic
owned adapter and jump assembly with the public context-header overlay.
`private_layout.cpp` is excluded. Build the framework under the new identity
`PrivateGuest.framework/PrivateGuest`; obtain the unsigned artifact before
signing and then run the native generator from this directory:

```
cargo run --locked --offline --features fixture-tools --bin public_fixture_resources -- PUBLIC_UNSIGNED_FRAMEWORK NEW_OUTPUT_DIR
```

It writes the fixed self-authored plaintext `default.xex` and the independent
27-key `OwnedGuestBinding.json`. Function addresses/configuration, identity and
layout are fixed from public declarations, not copied from the loaded API.
The caller must ensure the framework argument came from the source-only public
fixture build; the generator does not authenticate source provenance. It accepts
no input XEX, game assets or keys. A local smoke invocation using an unrelated
self-authored LLD Mach-O tested resource generation only, not loading/execution.
The later SDK case copies the public XEX into a new sandbox game directory,
prepares strict policy 0, verifies starting before renderer attachment is
refused, attaches Metal, starts, and checks main exit plus numeric counts and
all false readiness/authentication flags. No general game-memory readback API
was added for that test.

## Direct Metal comparison slice (development, 2026-10-25)

The live game still uses the existing Xenos/WebGPU/wgpu-Metal renderer. The
authored `constant_backend_fixture` is a separate offscreen public SDK probe;
it never attaches to the game, shares its GPU handles, or selects a fallback.
It is a first replaceable boundary, not a finished native Skyrim renderer.

Both paths consume the same binned integer records and values, using the current
constant-patch algorithm. The native path has its own Metal device/queue, offline
`MetalConstantReplay.metal`, shared buffers and ordered blit consumer. The WebGPU
path uses the production WGSL/layout with probe-only reset/readback resources.
Rust record-major scalar replay is the independent byte oracle; the actual Metal
lane core is also compiled as C++ for portable checks. Neither accepts game input.

Pipeline creation and two warm-ups per backend are excluded. Before **every**
warm-up/sample, reset blocks to 0xa5 and poison readback to 0x5a, with completion
outside timing. This detects a missing dispatch or copy instead of accepting old
correct output. Six sizes (6/27/37/68/136/1024 records), interleaved stages,
shrinking/growing/empty blocks, unchanged values, high mask bits and NaN payloads
are checked over their full output extents, including gaps. Canonical input
validation rejects overlapping writes, malformed indices, masks and extents.
The native/Rust limits are compared before GPU execution.

Three balanced ABBA/BAAB pairs retain all 72 samples. Timings are host elapsed,
not GPU execution or game FPS. Phase boundaries differ between APIs and must not
be equated individually. Native upload directly writes shared Metal buffers;
wgpu chooses its staging/storage strategy. Therefore this compares complete
implementations, **not** isolated API overhead with equal storage proven.
Readback/poisoning and a wait after every sample differ from real frame overlap.
No winner, phone performance claim, or automatic backend choice is made.

`GraphicsPresentationTests.testPublicOffscreenMetalPresentation` retains all
existing presentation/alpha checks and additionally runs the constant comparison.
The caller supplies an independent 25-second process deadline. Rust target
checking is not Objective-C++/MSL compilation or an Apple runtime test.

The next boundaries are: immutable API-neutral draws with exact resource/version
ownership; direct Metal shader bindings/pipelines; ordered passes/resolves/copies;
GPU-completion-owned recycling; finally presentation. Reuse Xenos parsing and
shader analysis. Preserve endian conversion, vertex fetch, clip/depth, blending,
discard/coverage, EDRAM aliases, load/store meaning and guest-visible fences.
An unsupported explicit Metal path must fail clearly; never silently call the old
renderer while reporting a Metal result. Keep the current path available for
matched comparisons until full-frame pixels, resource lifetimes and game scenes
are verified. Fixed game timings, streaming policy, LOD and saves are unchanged.

Source-backed comparisons and the migration rationale are recorded in
`build/native-metal-comparison-v1/research.json` at the lab root. The saved GTA V
5-star source, current Theft4/MCLA, GLUE 5, Muguet, NFS MW and GoldenPad were
surveyed; only architecture/algorithm ideas were adopted, without transplanting
title-specific hooks or treating other projects' FPS as Skyrim evidence.

## Play-quality changes (2026-11-11, unverified on a physical iPhone)

These address frame-time instability, hitches and unintended transparency. All
were type-checked for `aarch64-apple-ios` and exercised by the portable suites
on a software Vulkan device; none has run on Apple hardware yet.

- Guest shaders are compiled with safe (IEEE) Metal math. The vendored
  `wgpu-hal` 29.0.3 differs from upstream only in `src/metal/device.rs`, which
  sets `MTLMathMode::Safe` and precise math functions. Metal's default fast math
  assumes NaN/Inf never occur and removes the Shader Model 3 `0 * Inf == 0`
  guards the translator emits, so affected pixels failed the alpha test.
- `NtReadFile` marks the destination pages written after the read as well as
  before it. The renderer consumes a mark before reading a page, so a mark set
  only before a slow read could be consumed while the page still held old bytes.
- Guest threads and the renderer thread run at the user-interactive QoS class.
- The executor asks the driver whether earlier submissions finished only when
  the answer can change its decision, instead of twice before every command.
- Backpressure waits yield and nap for 100 microseconds before falling back to
  one-millisecond sleeps.
- The live device's resident-memory refusal thresholds are 3 GiB of textures and
  1 GiB of buffers instead of the 512 MiB fixture bounds, which the texture
  cache and supersampled EDRAM targets could exceed (a fatal renderer stop).
- Frames are presented with `presentDrawable:afterMinimumDuration:` once the
  title's swap cadence is established (`PresentPacer`), so a frame that finishes
  early no longer shortens the previous frame's time on screen.

Not addressed: pipelines are still compiled synchronously on first use, so a
shader combination seen for the first time can still cost a frame or more.
