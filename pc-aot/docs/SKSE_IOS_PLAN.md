# SKSE compatibility on iPhone — enablement plan

Status 2026-11-19: plan only. Nothing here is enabled. The current build
contract (`ios/runtime-port/ios_build_contract.h`) still rejects
`SKYDREAM_SKSE` / `SKYDREAM_SKYUI`, and `ios/runtime-port/CMakeLists.txt`
still filters `runtime/skse_*.c` out of the runtime.

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

## Steps for the iOS build

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

- Works when enabled: SkyUI (partial), MCM menus, Papyrus-only mods whose SKSE
  functions are implemented.
- Never works: SKSE DLL plugins (RaceMenu's skee, Crash Fixes, etc.), mods that
  store data in SKSE co-saves.
- The MOD list marks each MOD with the SKSE functions it would need that are
  not available yet.
