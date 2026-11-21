# Classic HDT HighHeels height experiment

This host fixture models one behavior of the owned HDT HighHeels beta 0.5 LE package. Its footwear script calls the regular Papyrus equip and unequip events. The active magic effect uses the plugin's native KeepUpdate and StopUpdate calls. The LE x86 plugin resolves the NPC node under the actor's current loaded skeleton. It updates the node's local Z by the difference between the desired and previously applied offsets, then refreshes world data. Removal subtracts the owned offset. On a skeleton replacement, the old offset belongs to the old skeleton. Foot and calf world positions enter the original calculation of the desired offset.

The [official Classic SKSE 1.7.3 source archive](https://skse.silverlock.org/) describes the named lookup and scene update in PapyrusNetImmerse.cpp, NiObjects.h, and NiTypes.h. A separate interpreter run with the owned pinned LE executable stops during DLL attachment at the game's BSFixedString constructor before SKSE Query or Load.

The fixture takes a caller-supplied offset, named-node resolver and commit callback. The commit requires the same root, node and observed height and treats the write and accepted refresh as one operation. The supplied generations must change when the root or node is replaced. A missing node, a nonfinite value or a failed commit preserves the previous ownership record for retry. Replacement drops the former offset for the new node. External additive changes to the same node remain in place when the owned offset changes or is removed.

The test uses an authored actor and node. The experiment is unlinked. Connecting equip/effect requests, identifying a serialized guest update point, proving stable node lifetime and write/refresh behavior, computing the offsets from the original effect, and testing in-game equip/removal remain separate steps.

The private task-queue fixture uses one of the two call sites reviewed in Classic SKSE 1.7.3 Hooks_Threads.cpp. It observes the original ProcessTaskQueue return at 0x69110B in the reviewed lift, then executes the original tail. The host test substitutes authored queue operations, compares the resulting guest registers, flags, stack, memory word and call order, and checks that only the opted-in invoking owner receives the notification. This host evidence covers the call boundary. An actual game trace still needs to establish scene ownership, reference lifetime, and the equip/effect request path.

The private item-delivery fixture observes the primary item-specific VM call in 0x8C9910 after return at 0x8C9B6A. On the exercised path the game has borrowed the actor reference before calling the VM and releases it afterwards. The observer records the phase captured before the call. The fixture compares equip and unequip with the original path and exercises an actor-only delivery with no matching item. It preserves the actor reference count, TLS word, registers, flags, and stack. The alternate item delivery and real VM execution still require a game trace. A notification by itself supplies neither a durable actor identifier nor the effect instance or magnitude. The owned scripts obtain those through their active magic effect and use KeepUpdate/StopUpdate after entering the effect. That edge has no witnessed runtime call because the owned DLL probe stops before SKSE registration. The item observer therefore cannot activate a height change.

The optional owner PEX check pins the actual LE beta 0.5 effect bytecode. It verifies the unqualified native state and types, including the instance getter GetMagnitude returning Float, global KeepUpdate taking ObjectReference and Float, and global StopUpdate taking ObjectReference. The parser verifies the two encoded KeepUpdate sites and one StopUpdate site. It also rejects a truncated copy in memory. Runtime values, actual native registration, and the retention of the effect or actor across a queue boundary are still unverified.

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
simulated contended owner releases it deterministically. A bounded
production observer still needs a proven call site where the engine
already made the snapshot, or a supported way to use the same guest lock
without waiting. The two stock snapshots do not prove lifetime across
those reads or callbacks. If OnEffectFinish arrives after the entry is
removed, live resolution can fail. A future cleanup needs a previously
authenticated instance and its world generation and equip lifetime.
No played PEX dispatch, guest hook, scene change or delay is installed.
