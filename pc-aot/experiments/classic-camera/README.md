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

An isolated diagnostic against the owner's LE 1.4 package and pinned image
recorded 49 proposed stores using a private initialization budget of 200,000
work units. The probe preserved the input hashes and issued no guest game-code
stores. The normal app budget remains 100,000. The app reports Enhanced Camera
as unsupported. Keep the archive and detailed diagnostic private.
[Original MOD release](https://www.nexusmods.com/skyrim/mods/57859)

The proposed sites at `0x748835` and `0x74883B` identify the vanilla
paired visibility update in `f_7487B0`. After a TLS-dependent deferral check,
the direct path borrows both roots via the actor virtual `GetNiRootNode`, with
selectors 1 and 0, and changes their culling bits in opposite directions. A
change updates the actor's view flag and scene transforms and continues through
`f_73D0A0`. The diagnostic also found proposed changes at the mode branch
`0x74889D` and the post-update call `0x74899B`. This is the vanilla
visibility switch. Its borrowed pointers do not extend through a draw.

The unload branch of `f_73A5A0` releases the prior arms root at
`0x73A69E`, calls its virtual destruction operation if the count reaches zero
and then clears the actor's stored pointer. A pointer from the visibility switch
can therefore become invalid before the slot records its absence. The optional
pinned-generation test runs these two original blocks against authored memory.
It checks complementary bits, preservation of the other flag bits, and the
release, destruction and clear order with a missing, singly held or multiply
held reference. It does not exercise a render pass.

## Scene updates and render traversal

The owned 1.4 archive intercepts two calls to `NiAVObject::UpdateNode`:

- `0x724255` in `f_723FE0` updates a reference's scene root, including the
  player's third-person root. The native call passes a zero-delta,
  zero-flags context. This route first handles a TLS-dependent deferral.
  The DLL may restore its previously cached player-body local translation
  and request a separate update before issuing the native call.
- `0x735028` in `f_734F50` updates the first-person root after an earlier
  third-person-root update. Both native calls pass the game's frame delta
  with flags `0x2001`. The DLL issues the native arms update before its own
  helper re-acquires the roots and may update the body again. Its translation
  cache and the earlier restoration path span separate scene updates.

The pinned `UpdateNode` routine `f_AAF320` calls the receiver's downward
update and then reads the current parent and updates its bound before
returning. It does not acquire a reference for the caller or wait for a
render pass. The hosted fixture exercises both native call contexts and a
parent attached, removed or replaced during the downward update.

A separate render phase in `f_69B810` gets the player arms with
`GetNiRootNode(1)`. After setting the culling bit before `f_698800`, the
engine clears that bit at `0x69B930` for the `f_C7A510` traversal and sets
only that bit again at `0x69B943`. The hosted fixture runs that original
block and verifies the arms bit during and after traversal, other flag bits
and the untouched third-person root. The native traversal iterates four
contexts. An inner loop in `f_C7A1E0` retains each selected list item around
`f_C78E70`, which removes matching context entries and compacts the list.
That reference covers list cleanup. The DLL's pre-render hook at
`0x69B96A` runs before the subsequent `f_69A9F0` phase, outside the observed
vanilla arms window. The test does not establish third-person root
ownership through the renderer's submission.

### Retained entries in the render lists

On the `f_69B810` frame path, `f_698800` calls `f_694FD0` before
`f_69A9F0`. The list builder walks scene-node child arrays and checks the
application-cull bit before appending eligible nodes. `f_7BB370` stores each
chosen pointer and increments that exact object's reference count. The frame
owns these list entries until its cleanup after `f_69A9F0`, where
`f_45F5E0` releases each entry and the caller zeros the list count.

The `f_69A9F0` branch at `0x69AB0C` passes frame lists at `+0xC8` and
`+0xD4` into `f_CB9E20`. Its `f_C8D8D0` calls read scene-list entries and,
on the flag-filtered path, test each entry's culling bit before dispatching
it. The synchronous `f_B080C0` path obtains a separate reference from the
culling context at `+0x168`, calls `f_B07F40` with the node argument, then
releases the context reference. That reference is to the context object.
The body node is held by this frame list only if its pointer is the
actual stored entry.

The link from `GetNiRootNode(0)` to those entries depends on the live scene
hierarchy. The pinned route shows selection of scene nodes, whose
descendants can include the player. A retained parent can outlive the
detach or replacement of the player root. An entry skipped by the earlier
`f_694FD0` filter also cannot enter the submitted list when a bit changes
during `f_69A9F0`. The owned 1.4 request at `0x69B96A` runs after that list
construction and has no matched restore on return.

An idle-body override needs live evidence of the exact root in the relevant
first-person pass, a retained reference to that root before its first
culling read, and a completion point after its final scene consumer.
Returning to third person or replacing the scene root must still restore
only the state owned by the override. The current app supplies the
transition and root observations while retaining the game's original rendering.

Use the same game version, save copy and ordinary on-foot scene for a Windows
reference run with the MOD off and on. Inspect the player camera slot and
record the first-person arms, torso and legs separately. Compare the transition
back to third person and verify that turning the MOD off restores the baseline.
Mounted and forced camera modes need their own comparisons before those
options can be enabled.

A body override needs a render-pass handoff with a live root reference and a
verified completion boundary before restoring visibility. Returning to third
person or replacing the root on reload must preserve the engine's final
visibility. The ordinary first-person test also needs to distinguish body
geometry from duplicate third-person hands, head geometry and shadows.

### Read-only retained list diagnostic

The optional handoff observer runs just before the original call to
`f_CB9E20` in the reviewed `f_69A9F0` branch, only when called from the
original `f_69B810` first-person frame path. The two list addresses come
from the game's prepared arguments, and the observer checks them against
the same frame. The camera's current state must match the first-person
slot before and after inspection.

Each list classification is a bitmask. Bit 1 means the body root is an
exact retained entry. Bit 2 means an entry's parent reaches the body root,
directly or through other entries retained by these same lists. Bit 4
means an entry's parent chain ended without reaching the body. Bit 8
means an entry or its ancestry could not be classified within the
observation scope. A zero mask is empty. Bits can appear together.
The reader inspects at most 128 entries per list and eight parent links
per candidate. It treats an unretained intermediate parent as unknown
without reading that object's memory. The body identity is read anew
before and after the scan. All guest addresses and sampled references
are discarded before the original call.

These results refer to retained entries passed to the original
culling routine. Actual draw submission and an ownership window for
a body-visibility change remain separate questions.
