# Classic camera state probe

This hosted probe reads the current player camera state after the original Classic `TESCamera::SetCameraState` returns. Its only input from the guest is a bounded read callback. The original call owns the state transition, reference counts and start/end callbacks. The fixture exercises the probe with the pinned image identity, the known entry and the matching player camera object.

## Verified boundary

The Classic SKSE 1.7.3 source describes the `PlayerCamera` singleton, current state and 13 state slots. The pinned Skyrim LE 1.9.32 PE and its matching local AOT lift confirm the transition entry and current-state field. The vanilla `IsPC1stPerson` handler also uses the singleton and embedded state table. Its answer additionally depends on earlier states and actor mode. The probe reports the current slot index. [Classic SKSE sources](https://skse.silverlock.org/) and [vanilla command list](https://skse.silverlock.org/vanilla_commands.html).

LogicDragon describes Enhanced Camera 1.4's ordinary first-person view as the original arms and a visible third-person body, with head bob disabled by default. Riding, transformations, furniture, weapons and field of view have separate rules. Verify drawn meshes by observing the scene. [Enhanced Camera 1.4 author notes](https://www.nexusmods.com/skyrim/mods/57859?tab=posts).

## Hosted check

The authored fixture covers first and third person, horse, dragon and the transition state. It also checks a different image, a different camera, a null or unlisted state, overflow and an unreadable slot. Run it from this directory with a C17 compiler.

```sh
cc -std=c17 -Wall -Wextra -Werror camera_state.c test_camera_state.c -o /tmp/classic-camera-test
/tmp/classic-camera-test
```

A separate local run used the exact lifted state transition with an authored camera object and simulated state callbacks. With the observer left off and then called after return, the lifted routine produced the same stack cleanup, state selection, reference counts and callback order. This check covers the one engine transition in a hosted fixture. The Windows MOD and iPhone rendering remain unverified.

## Enhanced Camera verification

The reported `VirtualProtect` diagnostic identifies an unresolved Windows import in the tested configuration. An initialization trace is needed to identify the DLL's requested address and whether it falls within the diagnostic's supported data-page operation. The needed input is LogicDragon's original LE 1.4 archive from the owner, including `SKSE_EnhancedCamera.dll` and `SKSE_EnhancedCamera.ini`, or attributable source for that exact release. Keep the archive and any captured traces private. [Original MOD release](https://www.nexusmods.com/skyrim/mods/57859).

With that input, fingerprint the archive and DLL, then verify the PE32 and SKSE entry points. Record the first unsupported call in initialization and check its requested span against the known module or guest image. Use the same game version, save copy and ordinary on-foot scene for a Windows reference run with the MOD off and on. Inspect the player camera slot and record the first-person arms, torso and legs separately. Compare the transition back to third person and verify that turning the MOD off restores the baseline. Mounted and forced camera modes need their own comparisons before those options can be enabled.
