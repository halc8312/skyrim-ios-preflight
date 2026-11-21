# SKSE compatibility on iPhone — enablement plan

On 2026-11-20 the normal app's test configuration includes the Classic v1
guest and a per-profile SkyUI 5.1 trial. That setting defaults to off. The
trial is selected at startup after the matching guest has been bound and
the profile has successfully published an enabled `SkyUI.esp`. The signed
normal build 18 and the newer diagnostic builds are tracked in [STATUS.md](STATUS.md).

## General compatibility for imported DLLs

1. Import the MOD from Files. The app validates the 32-bit Windows image and
   follows named imports to reachable helpers in the same package. The
   diagnostic uses a disposable guest address space and runs only when opened.
2. Grow shared Windows and SKSE services around verified contracts. The
   diagnostic already has file snapshots, basic Windows synchronization,
   one logical worker, the Classic serialization callbacks and the two task
   queues. A test reports the operation that prevented the run. The app can
   exercise the supported part without a per-MOD conversion step on a PC.
3. The resident DLL test owns its images, registry and queues across separate
   host calls. The Classic trial can record the first returned Papyrus
   registration and menu hook, with the observed order and whether each ran on
   the startup thread. For PapyrusUtil 3.3 the hosted trial also captures typed
   declarations and matches compiled script calls to verified DLL wrappers.
   Connect those wrappers through the owning registry epoch and verify its VM
   and game-object lifetimes on iPhone. [Typed trials](../experiments/skse-plugin-vm/README.md)
   The optional launch probe now checks the selected PapyrusUtil 3.3 DLL and
   PEX, resolves the seven tested JsonUtil signatures and prepares a read of
   Wearable Lanterns' common settings. The hosted backend stages each save
   until the caller acknowledges its result. On the pinned LE build, an
   ordinary load can continue after the VM freeze times out. Registry reset
   can pop frames even with a retained script stack. Gameplay admission
   requires a valid frame before argument access and through the engine's
   own result copy.
   [Settings backend](../experiments/skse-plugin-vm/classic_settings.h)
4. Validate an additional save's structure inside the selected profile. The
   hosted round trip runs the pinned fresh-save and fresh-load calls through
   the prepared file wrappers. The companion is eligible after the base file
   is sealed and the VM confirms the matching loaded generation. Cached
   preload and live app integration remain open. [Save round trip](../experiments/classic-save-roundtrip/README.md)
5. Extend the D3D9 bridge where rendering plugins need real device state and
   presentation callbacks. Verify output and GPU time against reference cases.
   The tested color treatment is one small rendering prototype. [D3D9 presentation experiment](../experiments/d3d9-present-grade/README.md)

## First-wave gameplay checks

Use the original LE distributions selected through the owner's Files app.
Record the installed versions, observable result and any failure for each
profile. Use copied saves for changes that persist across a relaunch. These
are the main-file versions listed by the [Nexus file API](https://graphql.nexusmods.com/#query-modFiles)
on 2026-11-20.

| Test | LE packages | Observable result |
| --- | --- | --- |
| SKSE and menus | SKSE Classic 1.7.3 scripts, SkyUI 5.1 | Open inventory and MCM using touch input. Change an MCM setting, quit and reload. |
| Papyrus and saves | PapyrusUtil 3.3, Wearable Lanterns 4.0.2 | Call a real StorageUtil function, use the lantern and restore a value from the same copied save after a relaunch. |
| Character editing | RaceMenu 3.4.5, ECE 1.59, NetImmerse Override 3.4.4 | In separate configurations, edit an appearance and verify a preset and appearance survive a reload. RaceMenu already includes NiOverride. |
| Hair and character menu | ShowRaceMenu Precache Killer 1.3, KS Hairdos 1.10, Apachii Sky Hair 1.6 Full | Select new hair and observe the menu's pre-cache behavior with the intended DLL loaded. |
| Dynamic meshes | HDT Physics Extensions 14.28, HDT HighHeels beta 0.5, XPMSE 5.06 | See movement on an HDT-enabled asset and check that the height offset applies and clears on equipment changes. |
| Legacy skeleton | XPMS 1.96, the matching Realistic Ragdolls and Force 1.9 files | In a separate profile, verify the skeleton is selected and transitions animate correctly. |
| Generated assets | CBBE 3.4f, UNP base 1.2, BodySlide 6.1.0, FNIS Behavior 7.6 | Verify base bodies in separate profiles, then import owner-generated meshes and behavior for the matching profile and observe the result. |
| Game data | USLEEP 3.0.16 with the three official DLC | Resolve the masters, load a copied save and verify an affected record or quest. |
| Camera and loading | Enhanced Camera 1.4, Safety Load 1.2 | Verify first-person body and camera behavior and exercise the affected loading path. |
| Unvoiced dialogue | Fuz Ro D-oh 6.1 | Open an unvoiced conversation with the original package. Verify subtitle duration and lip motion in the menu and world chatter, then check an ordinary voiced line. |

Compare startup, frame times and save behavior to the same build with the
test profile disabled. Match the signed app to its guest ABI and verify
owner-only delivery after the gameplay paths are available.

Current host checks cover PapyrusUtil registration, the Wearable Lanterns
configuration and startup settings, and integer and text records restored by the
original DLL in a new process. An opt-in named profile now connects the companion
writer to successful base-save publication and checks the selected base stream
before restoring records to the corresponding VM. The native actor reference
and the managed VM identity have separate ownership scopes. Authored actor
records pass the hosted tests. Live NPC StorageUtil calls and iPhone save
recovery still need verification.

The other host checks cover Wearable Lanterns input and meter notifications,
separate RaceMenu and ECE imports, deployment recovery, selected DLC and generated
asset placement, and both Fuz subtitle routes. The fixed ECE 1.59 DLL and menu
confirm how the slot version changes the value passed to the stock Nose Height
slider. Applying it still requires the current preview and selected TRI to match.
The isolated DLL file probe completes ShowRaceMenu Precache Killer 1.3
initialization. The startup-view check recognizes the exact DLL and enables
the verified guest precache return only for the selected profile. Enhanced
Camera 1.4 completes isolated PC initialization when the diagnostic work
limit is raised for that run. It records 49 requests to change game code, with
none applied. The Safety Load 1.2 experiment recognizes the exact DLL and uses
the reviewed allocation branch during the loading menu. HDT HighHeels beta 0.5
has host tests for typed VM arguments, a retained target during its stock native
effect event, and applying and removing the node offset in a separate fixture.
The integration still needs the current actor and scene node to be held through
one update. These tests use the existing diagnostic work limit outside the
separately documented Enhanced Camera experiment.
[Progress](STATUS.md)

## RaceMenu LE appearance editing

The first target is RaceMenu 3.4.5 on Skyrim 2011 LE 1.9.32. The preset
staging entry point is
[`skyrim_mod_stage_racemenu_preset`](../ios/runtime-port/mod_deploy.c).
It copies a selected `.jslot` into
`Data/SKSE/Plugins/CharGen/Presets/` in the staged MOD package. The launch
view receives that package when its profile enables it. The ordinary app's picker calls the same entry point and attaches the
staged package to the profile selected at import.

The current test uses authored bytes to verify placement, safe filenames,
preservation of an existing preset and separation of the two launch
views. The test treats the preset contents as an opaque import fixture.

```sh
cd pc-aot
python3 -B -m unittest discover -s tests -p test_mod_archive.py -v
```

The author source identifies Classic 3.4.5.0 as `chargen.dll`. Its serializer
stores the ordinary float options in `morphs.default.morphs`, in the order of
`TESNPC::FaceMorphs::option[]`. Custom slider records use `morphs.custom`.
The Scaleform load route applies the preset to the current player through
the CharGen morph handler. [LE version resource](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/Resource.rc#L53-L71)
· [LE JSON writer](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/MorphHandler.cpp#L2091-L2107)
· [LE load route](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/ScaleformFunctions.cpp#L95-L132)

Nose Height is the first candidate. CharGen associates `sRSMNoseHeight`
with `FaceMorphList::kMorph_NoseDownUp`. The Classic SKSE 1.7.3 SDK's
`GameData.h` puts it at index 1, and `GameObjects.h` defines 19 float options.
Use `morphs.default.morphs[1]` in an authored parser fixture. A pair saved
from the same LE 3.4.5 character with just Nose Height changed can validate
the actual preset files. [LE slider labels](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/ScaleformFunctions.cpp#L239-L242)
· [Classic SKSE 1.7.3 SDK](https://skse.silverlock.org/)

The pinned LE lift shows the stock continuous-slider callback obtaining
the player's current actor base and reading and applying the option through
the engine's slider path. The first hosted adapter can use this existing
callback behind a test double. It should require the same selected profile,
open menu generation and current preview actor/base at application, plus a
finite value inside the live slider descriptor's range. Menu lookup yields
a borrowed object. Run the real call on the menu's owner thread after its
lifetime is verified. Keep owner presets, generated game code and raw game
traces private. The iPhone checks cover the head refresh and appearance
reload in a copied profile.

## What already exists upstream

The pinned skydream runtime (`vendor/skydream`, `ecf814f`) contains an
independent, partial classic SKSE Papyrus layer for TESV 1.9.32
(`docs/SKSE_COMPATIBILITY.md` there): 90 non-latent registrations (Math,
StringUtil except Split, ColorComponent, SKSE version queries, UI get/set/
invoke, menu/key/mod-event registration, Input.GetMappedControl, numeric INI
access), native Scaleform objects and callbacks, extended inventory data and
translation loading. The upstream tester reports SkyUI 5.1 inventory and MCM
working on Linux, with settings persisting. Favorites, active effects and
crafting menus are not validated. Windows DLL plugins and SKSE co-saves are
not supported and cannot become supported by this route.

It does not load `skse_1_9_32.dll` and copies no SKSE code; keep it that way
(the SKSE package forbids redistribution of modified versions).

## Original iOS enablement outline

1. **Build variant.** Add an explicit option (for example
   `SKYRIM_IOS_SKSE=ON`, default OFF) that keeps `runtime/skse_*.c`, defines
   `SKYDREAM_SKSE=1` (and `SKYDREAM_SKYUI=1` for the SkyUI variant), and lifts
   the contract `#error` only for that variant. Keep the default build
   byte-identical to today.
2. **Papyrus registration.** Upstream wraps the original registration symbol
   at link time on Linux. ld64 has no `--wrap`; do the equivalent through the
   private guest binding: rename the generated registration function the same
   way `tools/prepare_skse_hooks.py` renames hook targets (`f_X` →
   `f_X_guest`) and provide the host `f_X` that calls the guest one and then
   `skse_register_natives`. Record the address in the binding manifest.
3. **Scaleform hooks.** Run `tools/prepare_skse_hooks.py` on the private
   generated guest C for the iOS translation (it is idempotent and checks
   that all 16 targets exist), then rebuild the guest framework. Public CI
   never sees generated code.
4. **Scripts.** SKSE's `.pex` scripts are needed by mods but may not be
   redistributed. The user imports their own classic SKSE 1.7.3 archive as a
   MOD package (Settings > MOD > Add); the importer keeps `Data/Scripts` and
   reports the DLLs as unusable, which is correct. Do not bundle scripts.
5. **Version reporting.** Upstream reports 0.1.0 by default; SkyUI checks for
   1.7.3. Ship the SkyUI variant with the upstream SKYUI define (which
   advertises 1.7.3) only after step 6 passes, and say clearly in the app that
   SKSE support is partial.
6. **Validation.** First the upstream self-test (`LB_SKSE_SELFTEST=1`,
   expects `primitive guest ABI probes passed` in the run log). Then, in a new
   MOD profile with a copied save: SkyUI 5.1 (LE) inventory, MCM page open/
   change/save/reload, and one Papyrus-only MCM mod. Note failures per menu.
7. **Coverage growth.** `tools/audit_skse.py` inventories the 798 classic
   registrations from a user-supplied SKSE source tree. The MOD analyzer in
   this app (`mod_analyze.c`) lists, per imported MOD, SKSE functions that the
   layer lacks; collect those names from real use to decide what to implement
   next, independently and without copying SKSE code.

## What the user will be told

- The opt-in trial covers part of SkyUI and the Papyrus calls its supported
  menus use. Test each menu and its saved settings on the target iPhone.
- Windows DLL gameplay, restoration of live `.skse` state and ENB presets
  are development work. Available checks are exposed in the app's diagnostic
  or save-check screens.
- The MOD list reports missing SKSE functions for imported packages.

## 2026-11-19 estimate for a separate experiment

Budget **2–4 engineering days** for an opt-in prototype that builds the private
guest with the pinned upstream compatibility code and passes its registration
self-test on an iPhone. Budget a further **3–7 engineering days** to exercise
SkyUI 5.1 inventory, MCM persistence and one Papyrus-only MOD on copied saves,
including a comparison against the ordinary build. These are planning estimates
assuming access to the owner's game and script archives, a Mac builder and a
test iPhone. A mismatch in the private guest bindings would require a new estimate.

The upstream runtime implements 90 native registrations and reports its SkyUI
tests on Linux. iPhone compatibility depends on the two device milestones above.
An arbitrary Windows DLL or a MOD depending on SKSE co-save serialization would
require a separate project. Build 16 keeps the existing compile-time exclusion.
