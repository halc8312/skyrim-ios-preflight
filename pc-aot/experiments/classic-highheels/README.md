# Classic HDT HighHeels height experiment

This host fixture models one behavior of the owned HDT HighHeels beta 0.5 LE package. Its footwear script calls the regular Papyrus equip and unequip events. The active magic effect uses the plugin's native KeepUpdate and StopUpdate calls. The LE x86 plugin resolves the NPC node under the actor's current loaded skeleton. It updates the node's local Z by the difference between the desired and previously applied offsets, then refreshes world data. Removal subtracts the owned offset. On a skeleton replacement, the old offset belongs to the old skeleton. Foot and calf world positions enter the original calculation of the desired offset.

The [official Classic SKSE 1.7.3 source archive](https://skse.silverlock.org/) describes the named lookup and scene update in PapyrusNetImmerse.cpp, NiObjects.h, and NiTypes.h. A separate interpreter run with the owned pinned LE executable stops during DLL attachment at the game's BSFixedString constructor before SKSE Query or Load.

The fixture takes a caller-supplied offset, named-node resolver and commit callback. The commit requires the same root, node and observed height and treats the write and accepted refresh as one operation. The supplied generations must change when the root or node is replaced. A missing node, a nonfinite value or a failed commit preserves the previous ownership record for retry. Replacement drops the former offset for the new node. External additive changes to the same node remain in place when the owned offset changes or is removed.

The test uses an authored actor and node. The experiment is unlinked. Connecting equip/effect requests, identifying a serialized guest update point, proving stable node lifetime and write/refresh behavior, computing the offsets from the original effect, and testing in-game equip/removal remain separate steps.

The private task-queue fixture uses one of the two call sites reviewed in Classic SKSE 1.7.3 Hooks_Threads.cpp. It observes the original ProcessTaskQueue return at 0x69110B in the reviewed lift, then executes the original tail. The host test substitutes authored queue operations, compares the resulting guest registers, flags, stack, memory word and call order, and checks that only the opted-in invoking owner receives the notification. This host evidence covers the call boundary. An actual game trace still needs to establish scene ownership, reference lifetime, and the equip/effect request path.

The private item-delivery fixture observes the primary item-specific VM call in 0x8C9910 after return at 0x8C9B6A. On the exercised path the game has borrowed the actor reference before calling the VM and releases it afterwards. The observer records the phase captured before the call. The fixture compares equip and unequip with the original path and exercises an actor-only delivery with no matching item. It preserves the actor reference count, TLS word, registers, flags, and stack. The alternate item delivery and real VM execution still require a game trace. A notification by itself supplies neither a durable actor identifier nor the effect instance or magnitude. The owned scripts obtain those through their active magic effect and use KeepUpdate/StopUpdate after entering the effect. That edge has no witnessed runtime call because the owned DLL probe stops before SKSE registration. The item observer therefore cannot activate a height change.

The optional owner PEX check pins the actual LE beta 0.5 effect bytecode. It verifies the unqualified native state and types, including the instance getter GetMagnitude returning Float, global KeepUpdate taking ObjectReference and Float, and global StopUpdate taking ObjectReference. The parser verifies the two encoded KeepUpdate sites and one StopUpdate site. It also rejects a truncated copy in memory. Runtime values, actual native registration, and the retention of the effect or actor across a queue boundary are still unverified.

### Compute the offset from the current legs

The exact beta 0.5 library selects the left and right Foot and Calf world
positions under the loaded skeleton. It subtracts Foot from Calf for each
leg, measures both lengths, and chooses the larger nonnegative ratio of
vertical rise to length. Starting with the sum of both lengths divided by
the binary32 reference length `55.8993988`, it multiplies by the NPC node's
local scale, divides by its world scale, multiplies by the effect magnitude
and that ratio, and adds the configured base adjustment. The swimming state
produces zero. Smoothing mode 1 limits change only in the selected movement
state. Mode 2 always limits it to the configured step around the previous
owned height. The comparison uses the fixed-library instruction and constant
checks, plus authored poses with different scales, slopes and movement modes.
The owner archive's INI supplies zero base adjustment, smoothing mode 1
and a maximum step of 0.1. The fixture also covers mode 2 and bypass.

`classic_heel_pose_height` consumes copied, finite pose data. A missing or
degenerate leg or invalid scale cannot produce a scene write. This math
function never resolves a node. Capturing the four points and committing the
offset still requires the same verified scene ownership window.

In the pinned LE lift, the attach blocker BSFixedString constructor calls the string hash/length helper and enters the game's intern pool before publishing a reference. The pool path uses the guest's shared initialization, lock and entry lifetime. The existing scalar frame helper is enabled only during an already-registered VM invoke. This fixture leaves both the attach path and pool untouched.

Run locally with a C11 compiler:

~~~sh
cd pc-aot/experiments/classic-highheels
python3 -B -m unittest -v test_highheel_height
SKYRIM_TEST_SANITIZERS=1 python3 -B -m unittest -v test_highheel_height
~~~

Set SKYRIM_TEST_OWNED_CLASSIC_LIFT to the reviewed private LE generation to include the task-queue and item-delivery ABI tests on a POSIX host.
Set SKYRIM_TEST_OWNED_HDT_EFFECT_PEX to the owner's unchanged data/scripts/hdtHighHeelEffect.pex from beta 0.5 to verify the effect declarations.

The owned DLL's GetMagnitude binding resolves an ActiveMagicEffect handle as
type 0x8B and reads its Float at offset 0x50. KeepUpdate passes a resolved
ObjectReference and Float. StopUpdate passes only the ObjectReference.
Neither static call receives the active-effect object.

The private hosted test runs the original LE effect handle construction,
type check, resolver, MagicTarget list traversal and matching effect visitor.
It samples the magnitude during the match and keeps only its Float bits.
The authored engine state covers a different active effect, a removed effect,
an unavailable actor, a non-actor and a wrong native type. The observer
preserves the original guest state. Connecting the script's live VM frame
to its effect lifetime, and observing its actual KeepUpdate and StopUpdate
dispatch, still need an authenticated runtime trace. This fixture installs
no native binding or scene change.

Set SKYRIM_TEST_OWNED_HDT_EFFECT_DLL to the exact owned beta 0.5 DLL and
provide GNU objdump to verify the three native boundaries without loading
the DLL.

### Associate the static update call with its script instance

The exact owned hdtHighHeelEffect PEX extends ActiveMagicEffect. MoveUp and
the scheduled OnUpdate call KeepUpdate with the target and a Float from
GetMagnitude on their instance. MoveDown calls StopUpdate with the target
alone. OnEffectFinish and OnSit can reach MoveDown, and the sit path can
unregister the scheduled update. StopUpdate therefore has no preceding
GetMagnitude to correlate. Several live effects can share a target. The
scheduled OnUpdate can also run on a different VM stack.

The fixed ordinary native invoke in `f_C46CB0` passes the current frame as
the last Run argument, and the owning stack's ID as the third argument. Its
ordinary Run return site is `0xC46E61`. The fixed frame push in `f_C3C8A0`
links the previous frame before publishing the new current frame, and the
pop in `f_C3C6C0` restores that link before cleanup. The private hosted
fixture executes the original invoke body with authored VM objects and Run
dispatch. While Run is active it validates the current frame and reads the
immediate caller's typed self. It exercises two distinct self values and
rejects a different registry or epoch, a mismatched stack, a displaced
current frame, an unexpected caller and a null self. Both paths leave the
same guest results, frame bytes and stack bytes.

The test supplies its function and class identities from the fixture.
Connecting this path to played PEX requires proving that the live VM
function and class belong to the exact winning script. A borrowed stack,
frame or VMIdentifier cannot be stored after Run. The stack ID only checks
the current invocation.

### Read the two typed update arguments during Run

The exact owner archive includes the effect and footwear PSC alongside
their PEX. Shoes add their configured spell after the equip checks and
remove it on unequip. The included effect script obtains GetMagnitude
from its own ActiveMagicEffect in MoveUp and scheduled OnUpdate.
OnUpdate schedules the next update after 0.1 seconds while raised.
MoveDown passes only the target to StopUpdate. The beta 0.5 ESM binds
hdtHighHeelEffect through the MGEF's VMAD. Its three shipped enchantments
use that effect with magnitudes 8, 11.8 and 15. Those records are examples
of effect data and do not fix the amount for other footwear. The
magnitude passed by the script is an input to the plugin's pose
calculation.

The exact DLL's KeepUpdate Run obtains the page with `f_C3A620` and
the two VMValues with `f_C3AC40`. Each VMValue has a type at +0 and
a payload at +4. Argument 0 carries an ObjectReference identifier to
the policy unpacker with type 0x3D. Argument 1 carries an IEEE-754
Float when its tag is 4. The DLL wrapper also handles integer and bool
conversions. The checked PEX's declared Float path is the scope of
this fixture. StopUpdate reads argument 0 only. Neither argument
contains the script's ActiveMagicEffect itself.

The existing owner-Run fixture now executes both stock argument
getters on authored inline and external VM pages. It verifies a
signed Float payload, reads the exact one or two argument slots
while the authenticated caller is current, and exercises missing
arguments, null or array identifiers, an integer in place of Float
and a nonfinite Float. The fixture reads the raw identifier during
Run and has no claim to its separate world-object lifetime. The
owned DLL checks pin the relevant wrapper instructions without
executing the DLL.

### Resolve the borrowed effect and compare the target

The fixed Classic registration for ActiveMagicEffect.GetTargetActor uses
`f_8DA360` for Run, `f_8D8F10` to unpack its base VMValue, and
`f_8D8500` to get the actor from the matching effect's MagicTarget.
`f_8D8650` first checks for a non-array identifier and asks the policy
whether its handle represents an ActiveMagicEffect. `f_C307C0` acquires
the identifier's guest lock, copies both handle halves and restores the
lock word. The stock unpacker takes a second locked snapshot, checks
whether the handle is valid, and sends the same typed value to the resolver.
The resolver checks the type again and traverses the target's current
effect list.

The private test executes those fixed bodies with authored VM and world
objects. In the matching visitor the test gets the stock target and
compares it to the supplied actor. Its Keep scenario also compares the
supplied Float with the effect's current magnitude. It exercises one
authored lock release, a different actor, a changed magnitude, a missing
target, a stale effect number, a removed effect, an unloaded or deleted
actor, an invalid handle, an array and a null identifier. The checks sample
the effect while the visitor sees it and retain no effect pointer.

The stock lock can sleep and retry without a deadline. The fixture's
simulated contended owner releases it deterministically. Its private
`f_C307C0` probe captures only the already-loaded handle halves immediately
before the original unlock. It takes no additional lock. An authored
handle change between the two stock reads leaves the guest result intact
and prevents the observer from accepting the pair. The stock Actor,
Character and PlayerCharacter MagicTarget slot for the effect list is
`f_6E9180`. It reads the process and its optional data before returning
the list. The fixture executes this getter, including the missing-process
case. Neither it nor the visitor acquires an effect-list lock or retains
the effect. The identifier lock therefore cannot establish effect
lifetime on another thread or across callbacks.

The owned PEX calls stock GetTargetActor in scheduled OnUpdate and certain
other handlers. Effect start and finish can receive the actor directly.
The snapshot observer consequently does not cover every Keep or Stop
through a preceding GetTargetActor call. If OnEffectFinish arrives after
the entry is removed, live resolution can fail. Cleanup needs a
previously authenticated instance and its world generation and equip
lifetime.

### Observe the stock effect event while its actor is retained

The pinned effect phase routine `f_656CB0` takes smart references to the
target and optional caster through `f_4952A0` before publishing the
start event (return `0x656EC5`, phase 1) or finish event (return
`0x65710B`, phase 0). It releases its slots after publishing. The common
publisher `f_656860` adds a second reference for each non-null object
while the original dispatcher `f_6EBC10` calls native listeners
synchronously. The event has caster at +0, target at +4, a 16-bit effect
number at +8 and phase at +0xA. Event copies release their references
when dispatch returns.

The private fixture runs the unchanged acquisition, publication,
dispatch and release bodies on authored handle entries and one native
listener. Interlocked imports and uncontended guard acquisition are
modeled. It covers start, finish, self-cast, optional caster, missing or
stale target, and a retained non-Actor. For an Actor it copies only the
FormID, effect number and phase while the target is retained. It checks
that all additional references are balanced. The no-target and
non-Actor inputs exercise the listener boundary independently of the
conditions in the original effect phase routine.

The stock VM listener `f_8D65B0` submits the corresponding Papyrus event.
The producer has its ActiveEffect in ESI and the target in a held stack
slot at both original publication calls. The optional owner ESM check pins
`hdtHighHeel.esm`, its `hdtHighHeelEffect` ScriptEffect and that record's
local FormID `0x800`. The upper byte at runtime must come from the loaded
plugin's actual index. An overriding record or replacement script needs
independent verification. Set `SKYRIM_TEST_OWNED_HDT_EFFECT_ESM` to the
unchanged LE beta 0.5 ESM to run this check.

The hosted identity join checks the original ESM and loose effect PEX in
the stable deployed Data directory and takes the load index from the
copied Classic DataHandler array for that launch. It checks up to 64 KiB
of each later active plugin for a complete TES4 header and treats a
dependency on the HighHeels ESM as a possible override. The result is
the expected EffectSetting FormID for a separately authenticated native
event. A device witness still needs to link the selected file to the
guest's successfully bound script in the current world.

The hosted PEX witness runs the pinned class loader, its original Store
open/read/close and the loose stream's original ReadFile route. The harness
models the parser's inner results and the caller's link result. It uses the
original final state write and admits only state 3 on the same pending class.
At the actual loose read return (`0xA580E2`), it compares the live descriptor's
bytes with the owner-supplied, digest-checked PEX without advancing the file.
Reused classes or stores, a different payload, a different same-name linked
type, a foreign thread, and parse or link failures produce no evidence.
Each case compares the original outcome, registers, flags, and guest fixture
memory with and without observation. Set `SKYRIM_TEST_OWNED_CLASSIC_LIFT` and
`SKYRIM_TEST_OWNED_HDT_EFFECT_PEX` to run it. The fixture does not establish
the effective script of an individual live VM object or enable HighHeels.

The private fixture now also runs the full producer on authored world
objects, including the fixed ScriptEffect phase callbacks and its
finish cleanup. It copies the retained actor's FormID, the EffectSetting
FormID, the effect number, the magnitude bits and the phase just before
native dispatch. The magnitude comes from the ActiveEffect field used by
Classic SKSE's GetMagnitude, with values distinct from the enchantment's
base magnitude. The cases exercise a missing target, a stale target
handle, a non-Actor, the wrong setting type, the right local ID in a
different plugin and a nonfinite magnitude. The original and observed
executions produce the same event, reference counts, CPU state, stack,
effect state and exercised call sites. The fixture uses authored objects
and substitutes for unrelated engine services.

The 16-bit effect number can be reused. A connection to the played
KeepUpdate or StopUpdate calls still needs to match the instance and its
world and equipment lifetime. Furniture and scheduled updates follow
their own script paths.

### Refresh the same named node after applying or removing the offset

In the exact owned DLL, refresh and undo each resolve the current
third-person loaded scene root, use its virtual name lookup for NPC,
and update that node's local Z. Apply adds the difference between the
desired and previous offsets. Undo subtracts the previously owned offset.
Both paths use the selected node's world-data slot at +0xB4. The fixed
NiNode and BSFlattenedBoneTree vtables point that slot to `f_AB53E0`.
Its direct path recomposes the world transform from local data and the
parent's world transform. Another path uses an auxiliary object and
dispatches separately.

The private scene fixture connects the height model to the original
NiNode name search and direct world-data path. It uses two authored
hierarchies with a named child and a scaled parent. One pose test resolves
both legs' `Foot` and `Calf` names through the lifted lookup, computes
the offset, refreshes world position and undoes the change. The fixture
also checks an independent change to the local baseline, rejection when
the root changes between read and commit, and a missing NPC name. The fixture's
scene is single-threaded and its nodes have fixed lifetimes. The exact
DLL instructions for local Z and refresh are also checked by disassembly.

The separate hosted traversal witness runs the pinned selector-0 block
in `f_734F50`, the original root getter and update, and the actual
NiNode child callbacks. It matches the stock interned NPC, Foot, and Calf
names only on receivers whose original parent callback and backlink both
belong to the current third-person root. A missing or preview root, missing
node, different parent, duplicate name, or exhausted visit budget prevents
the result. Under `BSFlattenedBoneTree`, the observer also distinguishes an
existing node consumed by the transform table from a node reached by
the child callbacks. It accepts the latter, including when the table also
uses it. It does not call the virtual name search, which can allocate and
attach a node when a flattened bone exists only as a record. The test
compares CPU state, scene memory and reference counts to the same original
execution with the observer disabled. It uses authored hierarchies and
does not establish serialization with renderer or animation workers.

With the optional `highheel_scene_observe` guest group compiled, the normal
app enables this diagnostic on the reviewed image when detailed diagnostics
are on and the active plugin list includes `hdtHighHeel.esm`. It samples
one stock player update. The selector path supplies the ordinary root;
preview and absent roots are skipped. The child callbacks supply the path,
and the original world update supplies the current node and its native
parent. The diagnostic copies only that current receiver's name token.
`IOS_CLASSIC_HIGHHEEL_SCENE` reports root update kind `0` unknown, `1` NiNode,
or `2` flattened. The `names` and `duplicate` masks use bits `1`, `2`, `4`,
`8`, and `16` for NPC, left and right Foot, and left and right Calf in that
order. `limited=1` marks a skipped or inconsistent branch or a visit limit.
The report contains no positions, object addresses, or file names. It only
counts actual path members visited by the reviewed world update and does
not establish that the effect applied or that later scene writes are safe.

A guest with the post-transform callback samples the local and world scales
from NPC and the world positions from the four leg nodes after the original
direct refresh stores. Each value is copied from the callback's own receiver
in the same traversal. The app evaluates the existing beta 0.5 formula with
a unit magnitude and no smoothing. The accompanying
`IOS_CLASSIC_HIGHHEEL_POSE` line reports which of the five inputs were
captured and whether the calculation accepted them. A missing or duplicated
node, an incomplete traversal, or a degenerate or nonfinite pose cannot
produce `valid=1`. Neither coordinates nor the computed offset are logged.
A names-only guest still reports the original scene diagnostic.

The official 1.7.3 `SKSETaskUpdateWorldData` takes a NiAVObject
reference in Create, calls the world refresh in Run and releases
the reference in Dispose. The queue's lock protects the delegate
queue. The pinned original `f_6A0B30` also balances its own queue
guard acquisitions before returning to `0x69110B`. The SKSE task
stores only the node pointer and therefore cannot
establish Actor ownership, current-root membership or serialization
with the rest of the scene. The acquired Actor references in the
effect-event fixture end with native dispatch. An application path
needs a bounded owner-thread window in which it can resolve the
current actor and root, take the required references and verify
membership through the write and refresh. The app's existing typed
Papyrus provider attests a VM frame and epoch, a different lifetime
from a scene root.
[Classic SKSE 1.7.3 source archive](https://skse.silverlock.org/)

The fixed player update obtains its third-person root in `f_734F50`
at return `0x734FCF`. It then passes that root to the original node
update at `0x734FF1`. Observing the receivers in that traversal can
establish membership from the child calls in the same update. The
stock transform reader in `f_ABDEC0` already visits materialized
flattened bone nodes. The virtual name search in `f_ABE390` can
create and attach a missing node, so it cannot serve as a passive
diagnostic. These observations leave exclusion against animation and
rendering writes unproven.

Played PEX identity, live third-person root ownership and serialization
with animation or rendering still need verification. The alternate
world-data dispatch is outside this fixture. The app's task queue,
runtime, and scene are untouched.
