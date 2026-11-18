# Source audit — Skyrim 2011 AOT foundation

Initial source audit: 2026-11-12. This is an internal engineering assessment of public source, not a gameplay certification. That initial audit downloaded no game inputs, APKs, desktop game binaries, dependency packages, signing material or cloud resources. Later user-authorized Steam installation and the two approved Python dependencies are recorded in STATUS.md. Source repositories remain pinned in `source-lock.json` and unmodified.

## Recommendation

Use **Skydream at `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a`** as the initial reproducible runtime foundation, retaining Skyrim_Android at `4cd7686a9a4d801a4176afbdae062a2cbecbb2bb` as a reference. Skydream has input manifests, embedded-image generation, synthetic regression tests, native media/D3DX replacements, and explicit desktop Apple portability work. Neither tree implements a validated iOS game port. Its inherited runtime/lifter licensing boundary also remains unresolved.

## Input contract and regeneration

### Read-only address evidence for a newly installed executable

Use `py -3.12 scripts/audit_pe_layout.py --exe "<completed-install TESV.exe>" --output local/pe-audit/<new-run>` only after installation completion is established. Optional `--reference "<independently obtained unpacked reference>"` compares identities without trusting that reference. The stdlib tool reads the 16 override addresses from the **pinned Git blob** of `tools/lift.py:152` through AST literals, never by importing/executing the lifter. It stores source revision/blob SHA-256, executable hashes/version metadata, bounded section metadata/full section hashes and 32-byte address-window hashes; no raw game bytes. Output is private and exclusive. `compatibility` remains `not_reviewed` even for byte-identical files. Exit 0 means evidence collected, not approved; malformed/unstable inputs return 2. A `.bind` image is retained as packed evidence and cannot supply hook-equivalence evidence. Absence of `.bind` alone does not prove unpacking correctness.

The exact upstream identity evidence remains Android's **original** SHA-1 prefix `f39aeceb` and version `1.9.32.0.8`, and Skydream's exercised SHA-1 `67a597df3bb8c5db94b9032f8179d29631b1145d`. Neither supplies an audited local full SHA-256, expected complete section table/entry point, or known-good bytes for these hooks. Skydream's `write_inputs.py` hashes the unpacked argument under the label `TESV.exe`. A legitimate separately obtained unpacked input therefore still needs original/derived hash-bound preparation provenance, exact version/layout verification and independent review of the fixed-address code/data contracts. A localized install with an older executable version must remain blocked even when every address maps into `.text`.

| Fixed-address assumptions in pinned Skydream | Source |
|---|---|
| `A60200`, `842140`, `873D70` (Scaleform hooks); `8F9720` (Papyrus entry) | `runtime/skse_gfx.c:284`, `runtime/skse_entry.c:5` |
| `F537F0`, `F52E30` (moves); `A4A4E0`, `A4A530`, `A4A570` (CRC) | `runtime/engine_primitives_bridge.c:43` |
| `F5B670`, `F520F0`, `F55A90`, `F59140` (byte-string operations) | `runtime/engine_strings_bridge.c:31` |
| `D0DB50` (timing); `D4EE40`, `D50ED0` (solver entries) | `runtime/physics_timing_bridge.c:59` and `:146` |
| Additional motion functions `D1DA60`, `D369E0`, `D64D70`, `D65860`; exact loads from `12D18DC` | `tools/prepare_physics_motion.py:8`; outside the 16-window audit |
| Timing globals include `1B4ADE4`, `12C2410`, `1B4ADF8`, `12D19E8`, `12D19C4`, `12D18E4`, `12D18E0`, `12D18DC`, `1BB5E20` | `runtime/physics_timing_bridge.c:67` onward; not a complete data-address inventory |

`tools/prepare_skse_hooks.py` requires all 16 generated function names but does not verify original machine-code behavior. `prepare_physics_motion.py` checks exact generated-expression counts for the four motion functions. `embed_game_image.parse_image` checks x86 PE32/base `0x400000`, 1–96 sections, `0 < entry RVA < image size <= 0x10000000`, bounded/nonoverlapping section mappings, no `.bind`, no exports and bounded imports/TLS; those are structural checks, not version approval. The new audit deliberately does not replace its import/TLS/relocation validation or CPU-semantic review. The Android snapshot has `HOST_OVERRIDES_SKYRIM = {}` at `tools/lift.py:152`; its release/source discrepancy prevents treating it as a known-good reference for Skydream's added hooks.

Synthetic bounds/fingerprint/provenance tests: `py -3.12 -m unittest discover -s tests -p test_pe_layout.py -v`. Game-specific reports stay under ignored `local/pe-audit/`; do not paste byte windows into tracked documents or populate `input-profile.json` from this audit alone.

| Item | Skyrim_Android pinned source | Skydream pinned source |
|---|---|---|
| Game | 2011 32-bit TESV 1.9.32.0.8; fixed-address hooks | 2011 / LE TESV 1.9.32; fixed-address hooks |
| Documented identity | README original TESV SHA-1 prefix `f39aeceb` | README exercised TESV SHA-1 `67a597df3bb8c5db94b9032f8179d29631b1145d` |
| Regeneration input | `TESV_unpacked.exe`, 32-bit `d3dx9_42.dll`, `binkw32.dll`, 32-bit `X3DAudio1_7.dll` | `TESV_unpacked.exe` only |
| Original DLL role | Four modules lifted; DLL images needed at runtime | D3DX native subset, Bink via FFmpeg, X3DAudio via F3DAudio; none of those three PE DLLs required |
| Runtime game data | Matching PE images plus `Data/` | Embedded TESV initialization/resources plus matching `Data/`; no external PE required |
| Python | 3.10+, Capstone 5.0.9, pefile 2024.8.26 | Same, pinned in `requirements.txt` |
| Extra differential dependency | Unicorn 2.0.1.post1 per README | Inherited `difftest.py` imports Unicorn; it is not in requirements.txt |

The two reported hashes are **not interchangeable**. Neither is a complete verified local compatibility profile. Skydream `tools/regen_gen.sh` passes the unpacked file to `tools/write_inputs.py`, which misleadingly stores its SHA-1 under the label `TESV.exe`. Keep separate original/packed and unpacked SHA-256, SHA-1, PE machine, image base, entry point, version and section metadata. Do not approve an input merely because its name/version matches. Unsupported SE/AE or unknown fixed-address layouts must fail before lifting.

Skydream regeneration runs discovery, lifting, exact-body deduplication, then `embed_game_image.py`, producing private `analysis/`, `gen/` and `gen/image/`. Its script removes stale generated DLL outputs; run it only in an isolated private build copy. It does not unpack SteamStub. Android source also still rejects `.bind`/SteamStub in `runtime/main.c:98`, despite later release notes claiming direct packed-input loading. Both generation scripts require the unpacked file by name. The initial source audit did not unpack inputs; subsequent actual English input preparation is recorded in [UNPACKING](UNPACKING.md).

Regenerate after changing input bytes, discovery/lifter semantics, generation flags or host override contracts; the Android README's “only when lift.py changes” is insufficient as a provenance rule. Preserve discovery rejects, unsupported-instruction details and unresolved target/import records. Fixed-hook lifting and game-derived execution tests remain gated on reviewed input contracts. Standalone generic discovery can inspect an unreviewed candidate without hooks, lifting or execution; its private runner and limits are described in [TRANSLATION](TRANSLATION.md).

Evidence: [Skydream input/build README](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/README.md), [Skydream regeneration](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/tools/regen_gen.sh), [Android README](https://github.com/ttom795/Skyrim_Android/blob/4cd7686a9a4d801a4176afbdae062a2cbecbb2bb/README.md), [Android loader](https://github.com/ttom795/Skyrim_Android/blob/4cd7686a9a4d801a4176afbdae062a2cbecbb2bb/runtime/main.c).

## CPU fidelity and failure behavior

- **Flags:** native-x86 differential tests reproduced the missing AF state in pinned `Flags`, arithmetic and flag transport. A pinned patch fixes defined AF behavior; expanded40,632-case comparisons pass at O0/O2 after13,016 baseline failures each. See [CPU_VALIDATION](CPU_VALIDATION.md) for exact evidence and limits. This does not fix existing function-local flags failing to persist across guest calls, or establish ARM64 equivalence.

- **Ordering:** `tools/lift.py:78` defaults `LIFT_MEMORDER` off; `LIFT_TSO` defaults on but maps shared reads/writes to volatile accesses unless `LB_STRICT_ORDER` is enabled. `desktop/CMakeLists.txt:149` defaults strict ordering off. The README's general acquire/release claim does not describe these defaults. For the initial fidelity baseline, record `LIFT_MEMORDER=1`, `LIFT_TSO=1` and `LB_STRICT_ORDER=ON`, then test ARM64 aligned/unaligned loads, atomics and publication patterns. Existing x86 host tests cannot certify Apple ARM ordering. The strict path uses acquire/release on aligned addresses and memcpy plus fences on unaligned addresses; this still needs validation.
- **x87:** `runtime/x86.h:34` stores the x87 stack as `double[8]`; `runtime/fpu.c` converts 80-bit loads/stores through double. Extended precision/exponent range is lost. The control word affects selected conversion helpers; this is not full x87 exception/status/precision behavior.
- **SSE:** `ldmxcsr`/`stmxcsr` in `tools/lift.py:936` only transfer `c->mxcsr`. Host arithmetic is not globally configured by that value. Rounding, denormals, FTZ/DAZ, NaNs and exception behavior need dedicated differential coverage.
- **Discovery/instructions:** the lifter rejects whole functions containing unsupported/junk/decode failures (`lift.py:1228–1253`); others emit `rt_unimpl`. Missing generated call targets and unresolved imports terminate with addresses/names in `runtime/dispatch.c:164–223`. Archive rejection counts/reasons; do not interpret successful generation as complete coverage.
- **False success remains:** `kernel32.c` returns success without implementing thread priority/affinity and `FlushFileBuffers`; `marvel_shims.c` implements an offline Steam stub; `xaudio2.c:138` logs unsupported audio formats and plays silence. These conflict with this project's strict failure requirement unless explicitly classified and replaced or rejected. A global “unknown imports abort” claim is insufficient.

The actual unpacked English candidate imports266 symbols from15 DLL names. `FlushFileBuffers`, `SetThreadPriority`, and `SetThreadAffinityMask` are present, so those runtime gaps are relevant to its declared import surface, although occurrence in the table does not prove execution. `LoadLibraryA` and `GetProcAddress` also mean static imports alone cannot enumerate the complete API surface. The private inventory is `private/translation/unpack-steamless-20261112-defaults/static-import-inventory.json`; no original DLL copying requirement was inferred from these names.
- **Differential limits:** inherited `tools/difftest.py` excludes many instructions, including div/idiv, control-word/MXCSR operations, CPUID/RDTSC and several atomic/stack/environment operations. Passing it would not prove full CPU equivalence. CPUID identifies a synthetic Pentium 4 and RDTSC returns monotonic nanoseconds (`dispatch.c:230–250`).

Evidence: [lifter](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/tools/lift.py), [memory access implementation](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/lift_prelude.h), [x87 storage](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/x86.h), [audio fallback](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/xaudio2.c).

## Renderer, Apple platform and memory boundary

The route is recompiled x86 D3D9 calls → guest/host ABI bridge → patched DXVK-native → Vulkan. Skydream's proposed Apple route continues through MoltenVK → Metal; it is not a native Metal renderer. Native vkd3d-shader compiles HLSL into legacy shader bytecode for DXVK; it does not make the game CPU code dynamic/JIT. DXVK enables portability enumeration/subset, but feature discovery does not emulate unsupported device features. Validate actual iPhone shader/format/render-target support and compare frames before any gameplay claim. The native D3DX implementation covers only 18 imported functions, with explicit limits for paletted formats, BC recompression, nested reflection and cube/volume BC emulation.

`CMakePresets.json` targets macOS 15+, Intel or arm64. Apple packaging assumes desktop dylibs, Vulkan Loader/ICD manifests, `@loader_path`, SDL desktop paths, and ad-hoc signing. There is no iOS preset, signed iPhone application target, lifecycle/storage bridge, validated touch/controller integration or iOS dependency closure in these audited build files. iOS SDK targeting, application lifecycle, sandbox data access, dependency linkage/signing and device graphics validation are separate work. A macOS arm64 executable or simulator result is not an iPhone build.

`runtime/main.c:572` reserves `1ull << 32` bytes. Guest pointers remain 32-bit offsets into `MEM`, so this requires a contiguous 4-GiB **virtual** region, not 4 GiB immediately resident RAM. POSIX uses anonymous read/write mmap, MAP_NORESERVE when defined, and a 64-KiB null guard. Darwin discard remaps pages to ensure zero fill. Windows has reserve/on-demand commit handling. Probe reservation, target page size/alignment, sparse commitment, faults, decommit/zero-fill and memory-pressure behavior on the physical iPhone before integrating game execution. Do not infer iOS allowance from desktop behavior or solve failure by silently reducing guest address coverage.

Evidence: [desktop platform limits](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/docs/BUILDING_DESKTOP.md), [D3DX scope](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/docs/NATIVE_D3DX.md), [guest mapping](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/host.c).

### MoltenVK primary-source check

Current `KhronosGroup/MoltenVK` main was resolved through GitHub to **`5189f8de86ec365860f63bf1da736815cfaea5cd`** (commit date 2026-10-13). Only metadata and text source/docs were read; it is pinned as a **research-only dependency**, not cloned, downloaded as a dependency package, built or integrated. Its README now says Khronos deprecated the implementation and community maintenance is best effort, while still recommending it for **iOS** and Intel Macs. The alternative KosmicKrisp recommendation is for Apple Silicon Macs; it is not evidence of an iOS replacement.

The precise shader route is **HLSL → native vkd3d-shader → D3D shader bytecode → DXVK DXSO compiler → SPIR-V → MoltenVK/SPIRV-Cross → MSL → Metal pipeline**. Existing D3D bytecode starts at the DXVK stage. There is no OpenGL stage in the audited Skydream route. Vulkan-to-Metal graphics/shader translation is distinct from x86-to-ARM64 CPU AOT translation.

Upstream README supports building the library with Xcode 15.0.1+ (its CI minimum) and recommends the latest public Xcode. The actual project sets `SDKROOT=iphoneos` and `IPHONEOS_DEPLOYMENT_TARGET=15.0`; these minimums do not establish iPhone 17 Pro Max/device-OS toolchain support. On an appropriate Mac, upstream commands are `./fetchDependencies --ios` and `make ios`, the latter selecting `generic/platform=iOS`; `make iossim` is separate. The fetch step obtains and builds external sources and needs separate authorization here. Dependencies are pinned under `ExternalRevisions/`; SPIRV-Cross currently resolves to `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`. MoltenVK's own license is Apache-2.0.

Use an iPhoneOS XCFramework slice, static or dynamic as appropriate to the final app linkage, rather than Skydream's macOS dylib package. Presentation needs `VK_EXT_metal_surface` backed by a `CAMetalLayer` associated with the UIView; the old `VK_MVK_ios_surface` is obsolete. Portability enumeration/subset negotiation remains necessary for a loader-based route. Do not enable private Metal APIs as an implicit workaround.

MoltenVK describes an almost-complete Vulkan 1.4 subset, with features/extensions conditional on GPU family, Metal and OS versions. That headline does not prove patched DXVK compatibility. Its documented limits include unsupported pipeline-statistics query pools, ignored `VkAllocationCallbacks`, and special PVRTC upload rules; direct linkage does not itself load Vulkan validation layers. Required texture formats, shader behavior, presentation and performance must be queried and measured on the target. For shader failures, retain original SPIR-V and translated MSL plus a Metal capture; do not disable effects or substitute successful no-ops.

Sources: [pinned README](https://github.com/KhronosGroup/MoltenVK/blob/5189f8de86ec365860f63bf1da736815cfaea5cd/README.md), [runtime guide and limits](https://github.com/KhronosGroup/MoltenVK/blob/5189f8de86ec365860f63bf1da736815cfaea5cd/Docs/MoltenVK_Runtime_UserGuide.md), [iPhoneOS build settings](https://github.com/KhronosGroup/MoltenVK/blob/5189f8de86ec365860f63bf1da736815cfaea5cd/MoltenVK/MoltenVK.xcodeproj/project.pbxproj), [platform Makefile](https://github.com/KhronosGroup/MoltenVK/blob/5189f8de86ec365860f63bf1da736815cfaea5cd/Makefile), [external source revisions](https://github.com/KhronosGroup/MoltenVK/tree/5189f8de86ec365860f63bf1da736815cfaea5cd/ExternalRevisions).

## Release/source correspondence and upstream evidence

GitHub connector metadata, release lists, recent commits/issues and referenced workflow runs were queried on 2026-11-12. This verifies the reported source/run metadata, not downloaded binary contents or independent gameplay.

| Evidence | Result |
|---|---|
| Android 0.30.15 tag | Points to pinned HEAD; last commit only changes README |
| Android 0.30.6 → HEAD | `git diff --stat 4a76ee250b2a23fc4222bf83deb510b5829ae861 HEAD`: only README, 3 deletions |
| Android build metadata | `android/app/build.gradle:18–19` still versionCode 23, versionName 0.21 / 0.21-dual |
| Android release discrepancy | 0.30.5 claims packed Steam loader; pinned loader rejects it. 0.30.15 claims water/float-emulation fixes; source correspondence is not established |
| Skydream latest public tag | `2026-11-09-desktop` → `55a9dc3d4d3fb40377ad5ce0d6cb74e65ab2c377`; pinned HEAD adds source CI cryptography installation |
| First full-game CI | Run 42591220037 success, source `e74a6b0e60b9bd8e93ed7919fa9946562203037f` |
| Windows package CI | Run 42624455501 success, source `93b3282f312fc3885541ff13d14db4367ffa9020` |
| Mac package CI | Run 42627345063 success, source `362219626b614295a8bbc93619344d2ec33f2163` |
| Dependency diagnostics | Run 42627498447 success, same `3622196…` source |

The Skydream release aggregates packages built at different revisions; a release tag is not sufficient proof that each package matches its source. No binaries were downloaded or executed. `docs/PRIVATE_FULL_BUILDS.md` explicitly limits the tests to relocated dependencies, native HLSL/Vulkan library loading and a small generated-code embedded-image/strchr smoke test. They do not enter gameplay or create a rendered game window. Its private CI upload/encryption workflow is documentation only here; this project has not uploaded any private inputs.

Recent Android reports include graphics/water defects (#91/#85), face color (#88), controller discovery (#93/#86) and localized voices (#84); they are user reports, not independently reproduced diagnoses. Skydream issues API returned no issues at audit time; that is not evidence of no defects.

Sources: [Android release](https://github.com/ttom795/Skyrim_Android/releases/tag/0.30.15), [Android packed-loader claim](https://github.com/ttom795/Skyrim_Android/releases/tag/0.30.5), [Skydream release](https://github.com/meizfl/skydream/releases/tag/2026-11-09-desktop), [Windows run](https://github.com/meizfl/skydream/actions/runs/42624455501), [Mac run](https://github.com/meizfl/skydream/actions/runs/42627345063), [private-build limits](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/docs/PRIVATE_FULL_BUILDS.md), [Android issues](https://github.com/ttom795/Skyrim_Android/issues).

## Dependencies and provenance

Skydream original contributions are GPL-3.0-only. Its `NOTICE.md` explicitly says the inherited Android runtime/recompiler lacks a top-level license grant and needs upstream permission; GPL does not resolve that boundary or license Bethesda-derived code/data. Keep game-derived outputs private. This audit does not make a redistribution determination.

Patched DXVK-native is identified as native-1.9.2b lineage and retains zlib notices plus separately licensed bundled headers. Android bundles SDL2 2.30.9; Skydream links system SDL2 and FFmpeg, whose exact build configuration/license must be captured for any future iOS build. Native spatial math is F3DAudio from FAudio 26.10, commit `6839b88e304a046ae1a609ff14a085371e02a4e0`, zlib; stb_image retains MIT/public-domain notices. vkd3d-shader 2.0 is LGPL-2.1-or-later. The shader builder pins archive SHA-256s for vkd3d, SPIR-V/Vulkan headers and JSON Perl, but **downloads them**; it was not run. Desktop package-manager dependencies are not an iOS reproducibility lock. Pin target-specific sources/configuration/patches before approved builds.

Full host builds need GNU-compatible C11/C++17, SDL2 development files, Vulkan headers, FFmpeg development libraries, pkg-config, CMake and Ninja. MSVC is expressly rejected. Native Windows uses MSYS2 UCRT64/MinGW; macOS uses matching Apple Clang/dependencies. Existing VS2022 does not satisfy this build. Generated units can require 1–2 GiB each; keep `LB_GEN_JOBS=2` and total heavy build jobs at 2 under the current resource cap.

Evidence: [NOTICE](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/NOTICE.md), [shader build source pins](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/tools/build-desktop-shader.sh).

## Local verification performed

Source acquisition, from `F:\SkyrimIOS`:

```powershell
git clone --depth 1 https://github.com/ttom795/Skyrim_Android.git vendor/Skyrim_Android
git clone --depth 1 https://github.com/meizfl/skydream.git vendor/skydream
git -C vendor/Skyrim_Android fetch --deepen=4 origin main
git -C vendor/Skyrim_Android diff --stat 4a76ee250b2a23fc4222bf83deb510b5829ae861 HEAD
git -C vendor/Skyrim_Android ls-remote --tags origin
git -C vendor/skydream ls-remote --tags origin
```

Both clones succeeded; source/tree IDs are in the lock. Git status was clean for both after testing. Windows Python 3.12 and WSL Python 3.12.3 each lacked capstone, pefile and unicorn (`importlib.util.find_spec` checked). WSL Ubuntu-24.04 has GCC 13.3.0; pkg-config is absent, so SDL/Vulkan/FFmpeg package availability was not established by that check. No package was installed.

The following exact command passed all **9** tests using existing WSL Python/cc and synthetic inputs only:

```powershell
wsl -d Ubuntu-24.04 -- sh -lc 'cd /mnt/f/SkyrimIOS/vendor/skydream && python3 -m unittest discover -s tools -p test_guest_memory.py -v && python3 -m unittest discover -s tools -p test_engine_primitives.py -v && python3 -m unittest discover -s tools -p test_engine_strings.py -v && python3 -m unittest discover -s tools -p test_deduplicate_generated.py -v'
```

| Suite | Result |
|---|---|
| Guest memory | 1 passed: optimized shared flag observes another thread |
| Native engine primitives | 3 passed: overlap/native pointers, CRC/fixed keys, signed translation index/stop |
| Native engine strings | 2 passed: pointer/byte contracts and guard pages, including 160,000 rounds across 16 threads |
| Deduplication | 3 passed: preserved calls/entry identity, literals/protected entries and semantic differences/state |

No full upstream game-generation, full runtime build, GPU test, ARM64 differential run, iPhone signing/install or gameplay test was performed. These nine passing host tests establish narrow source regressions only; the independent diagnostic iOS app's Apple results are recorded separately in PROBE.md. Later generic discovery and a32,000-case comparison of four isolated, diagnostically lifted original string functions are recorded in TRANSLATION.md; those private outputs are not a game build or approved profile.

After the user approved Capstone5.0.9 and pefile2024.8.26, their exact official PyPI wheels were hash-verified and installed into a private WSL environment. `requirements.translation-linux-x86_64.txt` pins both hashes; neither library is an iPhoneOS dependency. The newly enabled synthetic `test_discover.py` (1 test) and `test_embedded_image.py` (2 tests) passed, covering serial/parallel function discovery agreement and PE bounds/import/TLS/native initialization. Logs are under `local/tests/source-with-approved-deps-20261112/`. No Unicorn package was acquired and no full instruction-semantics certification follows.

Capstone's distribution metadata says5.0.9 while its Python `__version__` says5.0.7. The installed module is byte-identical to the [official5.0.9 source's stale CS_VERSION_EXTRA=7 constant](https://github.com/capstone-engine/capstone/blob/022575848782a4801fd150fdbc927effcbca0864/bindings/python/capstone/__init__.py); [pkgconfig.mk](https://github.com/capstone-engine/capstone/blob/022575848782a4801fd150fdbc927effcbca0864/pkgconfig.mk) supplies distribution patch9. The loaded native library and METADATA also exactly match the approved wheel. Preserve this distinction rather than editing constants or changing the lock. Full local evidence is `private/tooling/wheels-20261112-skydream/capstone-version-audit.json`.

## Other iOS AOT ports inspected on 2026-11-12

These are primary-source project claims and public records, not device tests performed by this project. No dependencies or game inputs were acquired from them.

| Project | Relevant evidence and limits |
|---|---|
| [NFS Most Wanted iOS](https://github.com/bep1s69/nfsmw-ios) | Xbox360 PowerPC to Apple ARM64 through ReXGlue; native Metal renderer. The README lists career/free-roam/save support and testing on iPhone17ProMax and iPadProM4. [Release0.6.0](https://github.com/bep1s69/nfsmw-ios/releases) is published. No performance numbers are adopted as our own measurements. |
| [Ben10 Protector of Earth iOS](https://github.com/rishwajeet/ben10-protector-of-earth-ios) | Wii PowerPC through DolRecomp to C/ARM64, with a Metal/runtime implementation. The developer reports playing the fort, canyon and final boss on iPhone18Pro and spot-checking other levels. It remains a developer preview, with only one Mac conversion environment reported. |
| [SunPad](https://github.com/chrissotraidis/sunpad) | Super Mario Sunshine AOT with Metal on a Dolphin-derived runtime. Its README distinguishes iOS interpreter fallback for uncovered code from Mac JIT fallback. Gameplay/import/input were exercised mainly on iPad; older-iPhone performance and save durability still need work. It is not evidence of a completely AOT-only runtime or equivalent Skyrim performance. |

These support the feasibility of AOT plus a platform runtime as an iPhone architecture. They do not validate Skyrim's x86 lifter, Windows/D3D9 compatibility, guest memory requirements or frame times. Their iOS lifecycle, input, diagnostics and renderer architecture may inform later design; the initial game input remains the owned Steam2011 version.

The separate [skyrim-apple](https://github.com/Legoless/skyrim-apple) project targets the **Switch** version. Its README reports recorded iPad gameplay at about5–6FPS, incomplete later-scene/shader coverage and unverified base-cartridge compatibility. Its [release page](https://github.com/Legoless/skyrim-apple/releases) has no releases. This is an experimental Apple runtime, not a verified playable Steam2011 iPhone build or an interchangeable input path. Public searches did not identify a verified released iPhone build of the Steam2011 static-recompilation route; that is a bounded search result, not proof that no private or unindexed project exists. No Switch input or change of the agreed game target is introduced.

[BCD](https://github.com/thebcd69/bcd-games) separately lists SkyrimSE iPhone operation, using Wine/FEX and runtime JIT. This is relevant evidence that other execution routes exist, but it does not meet this project's no-runtime-x86-JIT objective. No installation, device benchmark or change of implementation route was performed.
