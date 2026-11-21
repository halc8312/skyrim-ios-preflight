# Classic control events

Official Classic SKSE 1.7.3 registers control listeners for `Form`, `Alias` and `ActiveMagicEffect`. Its `PapyrusEvents.cpp` observes the game's `ButtonEvent`. It queues `OnControlDown(string control)` when flags are set and the timer is zero, and `OnControlUp(string control, float holdTime)` when flags are clear and the timer is nonzero. `GetControlID()` supplies the action name. `Activate`, `Sprint` and `Ready Weapon` are control names in the supplied `Input.psc`. [SKSE Classic source archive](https://skse.silverlock.org/) · [SKSE 1.6.2 change notes](https://skse.silverlock.org/skse_whatsnew.txt)

## Xbox and touch handoff

The iOS overlay's A button sets bit `0x1000` in the `SkyrimMobilePad` snapshot. The prepared `input.c` merges that snapshot into slot zero, where it also reads SDL's physical controller buttons. SDL Xbox A sets the same bit. The XInput 1.3 shim exposes that state to the guest.

The reviewed TESV 1.9.32 image's `BSWin32GamepadDevice` checks XInput in `f_A6B370` and creates `ButtonEvent` entries through `f_A6A940` and `f_A69A20`. `f_A68F20` applies the active input context and mapping in `f_A66EC0` before event dispatch in `f_6EBC10`. The resulting action is the `BSFixedString` at `ButtonEvent + 0x10`. The virtual accessor `f_667000` returns that field. Both physical and touch input use the game's current binding, so remapping A changes the action observed by both.

The pinned `skse_input.c` sink already reads the mapped name and sends each accepted edge to Scaleform. The Papyrus key-registration path is separate and polls keyboard state. A control-event adapter can use the existing engine sink, preserving Scaleform and avoiding a second mapping table or another polling path. A queued delivery must own the action name while the engine's event exists, since the Scaleform callback can itself finish a remap. [Pinned input observer](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/skse_input.c) · [Pinned Papyrus dispatch](https://github.com/meizfl/skydream/blob/ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a/runtime/skse_system.c)

## Hosted proof

The test stages the reviewed input observer in temporary storage and executes both its original callback and an added copy of each mapped edge before Scaleform. It exercises the exact private `GetControlID` accessor. Authored events cover remapped gamepad buttons, the same action from keyboard and gamepad, press, hold and release, unrecognized inputs, and a Scaleform callback changing the borrowed payload. It checks the original GFX callbacks still receive the same sequence and the additional observer retains the precise action and hold timer.

Run `python3 -B -m unittest -v test_control_events` with `SKYRIM_TEST_OWNED_CLASSIC_LIFT` set to the reviewed private generation. `SKYRIM_TEST_SANITIZERS=1` enables ASan and UBSan where available.

## Hosted registration and VM delivery

Classic keys control subscriptions by interned `BSFixedString` identity. Listeners for the same key are visited by ascending full 64-bit VM handle. The bridge holds one handle reference and one action reference per pair. Repeated registration and removal are idempotent.

The fixture runs the reviewed `skse_compat.c` native dispatch, `skse_system.c` base-handle extraction and the original handle policy slot calls against an authored VM. It exercises `RegisterForControl`, `UnregisterForControl` and `UnregisterForAllControls` for all three receiver classes. The authored queue copies each action before Scaleform can reuse the borrowed data. It validates event names, one string argument on press, and string and float arguments on release. Held samples queue nothing. Each edge uses the current subscriptions, so a listener added between press and release can receive release. An earlier queued event retains its own handle after unregistration in the fixture.

The original lifted `f_8D6550` calls the guest sleep cleanup at `0x8D657A` and resumes at `0x8D657F`. The staged callback captures the full handle before the call, lets the original call return and removes the matching subscriptions before lifting continues. The test compares the original and staged sequence and rejects the wrong VM. The revert test resets the authored VM queue and handle policy first, then clears the bridge. This matches the order of Classic's revert hook, which invokes the original `f_8D5DF0` first and clears the registration map afterward without calling the old handle policy. [SKSE Classic source archive](https://skse.silverlock.org/)

## Integration boundaries

- The existing Classic overlay can add the nine method descriptors to `skse_compat_values.c`, route their operations in `skse_system_run` and retain the guest interned strings. The common `skse_register_natives` path already sets the NoWait flag. The `skse_input.c` capture point is immediately before the existing GFX call.
- The native operations, observer and lifecycle callbacks need one VM lifetime and serialized access. The experimental coordinator is single-owner. The app must verify its synchronization and use the existing VM `QueueEvent` bridge to copy the typed values before returning to GFX. The hosted queue is authored.
- Classic uses the `0x8D69A7` load revert and the `0x8D6E16` script-reload revert. The separate fresh-load fixture exercises the normal-load call site. The registration fixture models the old-world reset and exercises the original form-delete caller; it leaves both actual revert handoffs and the `CTLR` co-save record open. [Fresh-load fixture](../classic-fresh-load/README.md)

## Wearable Lanterns LE

The 4.0.2 SkyUI configuration script registers the selected action and toggles the lantern through a delayed Papyrus update when that action stays held outside menu mode. Release cancels the pending toggle. `Activate`, `Sprint` and `Ready Weapon` are the configured choices covered by the observer and registration tests. Verification in the game still needs to show one lantern toggle on qualifying holds from each input source, no toggle on a short tap or with a menu open, and registration continuity after loading. PapyrusUtil's profile and startup persistence are separate from this input path. [Wearable Lanterns 4.0.2 script](https://github.com/chesko256/WearableLanterns/blob/ebf678e6a17c5f3ee0723e851e1ff4497e77fe19/Scripts/Source/_WL_SkyUIConfigPanelScript.psc#L1157-L1188)
