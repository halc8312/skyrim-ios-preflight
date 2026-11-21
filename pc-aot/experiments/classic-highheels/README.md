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
