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
3. Connect enabled plugins to the game through checked handles and the
   actual game and UI callbacks. Verify their thread ownership, lifetimes
   and timing on iPhone before running them during play. Keep slow work on
   an appropriate worker and measure CPU time on the game thread.
4. Validate an additional save's structure inside the selected profile.
   Restoring live MOD state also requires evidence that the companion
   belongs to the completed base save and an owning VM that accepted that
   load. [Additional-save experiment](../experiments/skse-cosave/README.md)
5. Extend the D3D9 bridge where rendering plugins need real device state and
   presentation callbacks. Verify output and GPU time against reference cases.
   The tested color treatment is one small rendering prototype. [D3D9 presentation experiment](../experiments/d3d9-present-grade/README.md)

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
