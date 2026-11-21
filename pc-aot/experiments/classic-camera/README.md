# Classic camera state probe

This hosted probe reads the current player camera state after the original Classic `TESCamera::SetCameraState` returns. Its only input from the guest is a bounded read callback. The original call owns the state transition, reference counts and start/end callbacks. The fixture exercises the probe with the pinned image identity, the known entry and the matching player camera object.

## Verified boundary

The Classic SKSE 1.7.3 source describes the `PlayerCamera` singleton, current state and 13 state slots. The pinned Skyrim LE 1.9.32 PE and its matching local AOT lift confirm the transition entry and current-state field. The vanilla `IsPC1stPerson` handler also uses the singleton and embedded state table. Its answer additionally depends on earlier states and actor mode. The probe reports the current slot index. [Classic SKSE sources](https://skse.silverlock.org/) and [vanilla command list](https://skse.silverlock.org/vanilla_commands.html).

The camera update `f_83C7E0` confirms the player actor singleton. The root getter `f_7350A0` reads the first-person skeleton from the actor and otherwise calls `f_4D52C0`, whose normal path reads the loaded scene root. The observation reads the two stored roots described by the SDK's `PapyrusNetImmerse.cpp`. It does not call either guest getter or use the special TLS override of `f_4D52C0`. The SDK's `NiAVObject` layout gives the parent and flags offsets. The pinned scene traversal `f_F3CA10` contextually skips roots with the low flag bit set. These observations identify root attachment and the low culling bit at this point in the transition.

LogicDragon describes Enhanced Camera 1.4's ordinary first-person view as the original arms and a visible third-person body, with head bob disabled by default. Riding, transformations, furniture, weapons and field of view have separate rules. Verify drawn meshes by observing the scene. [Enhanced Camera 1.4 author notes](https://www.nexusmods.com/skyrim/mods/57859?tab=posts).

## Hosted check

The authored fixture covers first and third person, horse, dragon and the transition state. It also checks a different image, a different camera, a null or unlisted state, overflow and an unreadable slot. Run it from this directory with a C17 compiler.

```sh
cc -std=c17 -Wall -Wextra -Werror camera_state.c test_camera_state.c -o /tmp/classic-camera-test
/tmp/classic-camera-test
```

The optional fixed-lift fixture compares the reviewed state transition with
and without the observer. The stager declares a read-only observer, captures
the original receiver, then calls it at the single return after the original
register and stack update. The fixture covers valid changes, an unchanged and
null state, and another camera object. It compares registers, stack cleanup,
reference counts and callback order against the original using authored
callbacks. From the repository root, supply the reviewed generation:

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/reviewed-generation \
  python3 -B -m unittest discover -s tests -p test_classic_camera.py
```

The `camera_observe` MOD hook group uses this stage in the private guest
build. For an app assembled with that group, turn on **Detailed diagnostics**
and relaunch normally. Only a verified compatible guest image enables the
observer. It records `IOS_CAMERA_STATE slot=<n>` for the first observed slot
and subsequent changes. For first-person slot 0 and third-person slots 8 and
9, it also records `IOS_CAMERA_BODY schema=1 slot=<n> arms=<n> body=<n>`
when the slot or either root classification changes. Both diagnostics omit
addresses and game text and remain off in a startup that disables MODs.

| Root classification | Observed state |
| --- | --- |
| 0 | Unavailable, including an absent player actor, an unreadable field or aliased non-null roots |
| 1 | Root absent |
| 2 | No parent, culling bit clear |
| 3 | No parent, culling bit set |
| 4 | Parent present, culling bit clear |
| 5 | Parent present, culling bit set |

The staged-transition fixture uses authored start callbacks to change both
bits. The observer samples after the start callback and preserves the flags,
guest registers, references and callback order. The iPhone check compares the
root observations with the visible arms and body while switching from third
person to ordinary first person and back. Parent presence and a clear bit
alone do not establish which geometry was drawn.

## Enhanced Camera verification

The isolated diagnostic on the owner's LE 1.4 package stops at `HeapCreate`
during DLL initialization. The app still reports Enhanced Camera as unsupported
and the observer leaves the original renderer and camera behavior intact.
Keep the archive and captured traces private. [Original MOD release](https://www.nexusmods.com/skyrim/mods/57859)

Use the same game version, save copy and ordinary on-foot scene for a Windows
reference run with the MOD off and on. Inspect the player camera slot and
record the first-person arms, torso and legs separately. Compare the transition
back to third person and verify that turning the MOD off restores the baseline.
Mounted and forced camera modes need their own comparisons before those
options can be enabled.

The transition has no retained scene-root reference or render-pass lifetime
guarantee. A body override needs a verified update point that owns a live
root from the visibility change through draw or restore, including a return
to third person and replacement of the root on reload. Visibility changes
may also require excluding duplicate third-person hands and head geometry.
