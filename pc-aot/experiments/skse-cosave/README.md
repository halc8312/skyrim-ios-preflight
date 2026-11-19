# Classic co-saves across quicksave generations

This C experiment pairs one complete SKSE Classic companion with one completed base-game save. It operates on caller-provided file descriptors and a private SQLite database. The normal app and the game save directory have no connection to this experiment.

## Save identity

The caller provides three nonzero, opaque 16-byte IDs. They identify a profile, a logical slot, and a particular completed game save. Two writes to a quicksave or autosave slot need different event IDs, even if their `.ess` contents are identical. The slot ID must remain stable when the same slot is reused.

The adapter checks the SHA-256 of the **entire** `.ess`, including its size. It uses `pread` on the supplied read-only descriptor, leaves its seek position alone and checks the file's metadata before and after hashing. The digest is fixed when the save completes and checked again at publication. A later load requires the original profile, slot, event and matching bytes. A generation absent from the database takes one metadata lookup and avoids reading the `.ess` file.

Official Classic 1.7.3 constructs the companion path from the save name. Its 20-byte header contains the format version, SKSE version, runtime version and plugin count. It carries no save generation or fingerprint. An existing Windows pair can be imported when its provenance is independently established. A basename match on an arbitrary pair cannot establish that provenance.

### Check two copied pairs on the local computer

[compare_pair.py](compare_pair.py) compares both members against a previously trusted original using full-file SHA-256. Select the original `.ess` and the received `.ess` in their respective local folders. Each must have one same-basename `.skse` sibling. The received pair may have a new basename. The script checks the Classic headers, opens all four files read-only, and verifies their identities again after reading. Windows also denies writers and renames while all four handles are open. The command prints only equality flags and sizes.

```sh
python experiments/skse-cosave/compare_pair.py "Original/Quicksave.ess" "Received/Renamed.ess"
```

Exit code 0 means both copies match the original bytes. Code 1 identifies a changed member, and 2 indicates an unavailable or invalid input. The checker cannot establish that an arbitrary original `.ess` and `.skse` were saved together. That evidence must come from the source system. The script creates no manifest and performs no import or file writes.

A local Windows → Linux → Windows transfer test used a 3,919-byte authored base header and a 4,353-byte companion written by the original 32-bit SKSE serialization functions. On Linux the existing Classic save inspector read all 255 authored plugin names, and the native companion inspector validated its record structure. After the return copy Windows found both digests equal and reproduced the same eight loaded-registration rows. This exercise covers local file transfer and leaves iPhone Files/provider behavior for device testing.

## Calling sequence

| Action | Native entry point and result |
| --- | --- |
| Open the private store | `skse_pair_open` opens or creates `classic-generations.sqlite3` in an existing private directory. |
| Start a save | `skse_pair_begin` starts an empty stage or copies a previously verified generation. Passing an unavailable or mismatched base returns an error. A new slot can inherit from an explicitly loaded generation within the same profile. |
| Import a trusted pair | `skse_pair_import` validates and copies the supplied Classic bytes into a stage for the specified event. |
| Update one owner's state | `skse_pair_replace` replaces that owner's full record list. Zero records remove its section. Unknown sections, their order and their raw bytes survive. UID 0 is reserved for SKSE. Any failed edit invalidates the whole stage. |
| Observe successful game completion | `skse_pair_pin` takes the event and the stable, read-only descriptor from that save. This freezes all staged records and captures the digest. On a game failure, cancellation or superseded event, use `skse_pair_discard`. |
| Publish | `skse_pair_commit` compares the completed generation with the pinned proof, then atomically stores the whole image. An identical retry returns `SKSE_PAIR_ALREADY`. A conflicting write to an existing generation returns `SKSE_PAIR_CONFLICT`. |
| Load one owner | `skse_pair_restore` clears the caller's reversible receiver, validates the whole companion and visits that owner's records. Missing, incompatible, corrupt and mismatched input leave the receiver empty. If a visitor stops after a partial read, the adapter clears it again and reports `SKSE_PAIR_STOPPED`. |
| Export interoperable bytes | `skse_pair_export` returns an owned copy of the exact Classic image. The caller may write it to a separately chosen location and must release the buffer with `free`. |

All public declarations are in [generation.h](generation.h). Stores and stages belong to their calling threads. Separate connections can publish concurrently. The storage transaction is short compared with digest calculation, which happens outside the write transaction. SQLite full synchronous commits and its journal provide crash recovery for the database. A SHA-256 checksum covers the three IDs, `.ess` size and digest, and the companion bytes. It detects corruption and accidental row mix-ups. Database availability, free disk space and filesystem durability remain operating-system dependencies.

The adapter only reads `.ess` data. Its transaction covers the companion's registry and bytes. Separate writes made by the original game are outside that database transaction. A crash between the game's commit and the companion's commit therefore leaves a generation with no companion. Its load must report that condition and clear the receiver. An older quicksave never supplies an automatic fallback.

## Plugin serialization streams

[plugin_records.h](plugin_records.h) exposes the Classic v4 record operations to explicitly registered, reversible test receivers. The registry rejects duplicate handles and UIDs. Saving calls eligible producers in handle order. It combines partial writes, preserves zero-length records, and merges all owners in one final copy. Existing unknown sections, the core UID and registered owners that supplied no save callback keep their original bytes. A participating owner can intentionally clear its previous data by writing zero records. A callback or stream failure invalidates the stage before publication.

Loading verifies the generation once for every receiver combined. It gives each present owner its own section in file order. Remaining load callbacks receive an empty cursor in handle order, including registered callbacks with no assigned UID. Reading clamps to the current record, and requesting the next record skips any unread remainder. Form and handle resolution use the core mod history from that same verified file. Missing generations bypass all load callbacks. A failed callback discards every receiver's private scratch state.

The project-authored guest calls this path inside its synthetic VM save routine. Live plugin registration, executable callbacks and VM ownership still require their own game integration. The test registry's reset function only clears caller-owned state.

An i386 Windows reference ran ten original SKSE 1.7.3 registration, stream and dispatch functions with authored callbacks and files. A native writer produced all 1,111 bytes of the Windows output independently. Both loaders agreed on eight callback and record rows for that file. Editing two owners produced a 1,148-byte file, preserved 1,049 bytes of a third owner's section, and gave nine matching rows on the native and Windows readers.

### Observing form deletion

[form_delete.h](form_delete.h) registers a callback on any known plugin handle, including one without a serialization UID. A new callback replaces that plugin's earlier one. The router starts from the current VM's explicitly supplied sequence. It forwards the original 64-bit value once for each new confirmed VM event, in ascending plugin-handle order. A later genuine deletion with the same numeric handle still produces a new event. The caller must supply evidence that the original sleep cleanup and Classic's registration cleanup already finished. A missing sequence, a changed VM or early notification leaves the cursor in place. The router only runs when explicitly called by its owner.

A 32-bit Windows check ran Classic's original `SkyrimVM::OnFormDelete_Hook`, registration removals and serialization callbacks with authored VM state. The underlying sleep cleanup ran first. The menu, key, control, ModEvent, action, camera and crosshair holders followed in that order. Eight references to the deleted handle were released across the seven families. The remaining unrelated menu registration and the deleted handle's NiNode registration survived. Three callbacks then received all 64 original handle bits in ascending plugin-handle order, including two plugins without UIDs. A second genuine native delete call repeated those callbacks and released zero additional references. This verifies a source contract for the future on-device producer. A missing object during save parsing supplies no such event.

## Resolving saved form IDs and handles

[mod_history.h](mod_history.h) rebuilds a lookup table from the official core owner's `MODS` record in a verified generation. Classic writes record type `0x4d4f4453`, version 0, one byte of plugin count, and a little-endian 16-bit length followed by raw name bytes for each plugin. The current catalog must supply the guest DataHandler's effective indices and names in the same byte encoding. The reader compares ASCII letters case-insensitively and all other bytes exactly. Windows-specific locale or encoding conversions need separate evidence.

`skse_mod_map_load` sets up the table once per accepted load. It can report an absent generation or owner, missing or duplicate history, malformed input, unsupported record version and an inconsistent current catalog. Individual resolutions distinguish an absent or renamed plugin, an explicitly incompatible entry, an ambiguous current name and an old index outside the saved list. The wire record contains only names, so it cannot establish a rename or detect a different plugin distributed under the same filename. The adapter requires the caller's evidence for incompatibility and never substitutes a similarly named file.

The completed table occupies 512 bytes. Sorting and matching use bounded temporary arrays once on load. Each subsequent resolution is a single indexed lookup with no allocation or file read. Form IDs retain their lower 24 bits. A 64-bit handle also retains its upper 32 bits. Classic uses slot `0xfe` as an ordinary full plugin index and passes `0xff` through unchanged. Remapping provides a numeric handle. The runtime must still establish its VM type and lifetime. Failed lookups leave the caller's output value untouched. Call `skse_mod_map_clear` on revert.

## Reading Classic event registrations

[menu_history.h](menu_history.h) reads UID 0's version 1 `MENR` menu registrations from a verified generation. It uses the preceding `MODS` list to resolve each numeric handle. The reader checks the complete `MENR`, `REGS`, `REGE` sequence before delivering any row to an optional read-only observer. A duplicate group, a malformed key or payload, an unknown version, and the resource caps of 4,096 groups or 65,536 raw handles each produce an explicit failure. The result distinguishes resolved handles, dynamic handles and the reason other handles could not be resolved. Applying a registration would also require the live VM's handle ownership and event-dispatch rules.

[registration_history.h](registration_history.h) supports the eight event families written by Classic 1.7.3. Open `skse_registration_view_open` once for an accepted generation. The view holds one verified companion and its mod map, so checking additional families adds no `.ess`, database or filesystem reads. Release it on revert or when changing generation or profile. Named keys and callback names are uninterpreted bytes, with the Classic reader's 256-byte limit. Numeric input and action keys retain all 32 bits. Input mapping to iPhone buttons is a separate runtime operation.

| Recorded family | Key and stored state |
| --- | --- |
| `MENR` and `CTLR` | Menu or control name and the registered handles |
| `MCBR` | ModEvent name and a separate Papyrus callback name for each handle |
| `KEYR` and `AACT` | Numeric input key or actor action and the registered handles |
| `CHRR` and `CAMR` | A single set of crosshair or camera handles |
| `NINU` | A single set of NiNode update handles. The 1.7.3 core writer emits `NINU`. Its core reader recognizes `NiNU`. `skse_registration_format` reports the original tag and whether that reader recognizes it. Both spellings can be inspected independently. |

Counts refer to raw saved memberships. The official loader drops unresolved handles and forms a set of resolved handles for each key. Passive inspection does not retain VM references, subscribe a handler or dispatch an event.

### Retaining registrations across a confirmed load

[registration_activation.h](registration_activation.h) brings the decoded registrations into a caller-owned VM handle policy on its owner thread. The separate authored guest produces a complete load call trace. The bridge requires a normal revert before the VM load returns, success from the VM and outer game load, a matching completed base-file descriptor, and the expected profile, language, slot and saved generation. The policy independently reports which exact load and epoch its VM accepted. Replaying an earlier success after another quickload is rejected.

Recognized families are validated in full before consulting the VM. Within a family and key, the first subscription for a mapped handle wins as in Classic's registration set. Named keys use the caller's VM name identity. Separate event keys may each own a reference to the same handle. Missing load-order entries and missing live handles have different counts. There is one cap of 65,536 saved rows across the participating families. No reference is visible to a visitor until all acquisitions finish. This module performs work only at a load, inspection or teardown.

If acquisition fails while the same VM is live, the adapter releases each acquired reference in reverse order. A VM that is temporarily unreachable leaves an inactive cleanup obligation for retry. An authoritative VM reset invalidates the former epoch and its handle table. In that case the adapter discards its local rows without calling the old policy. Implementing these callbacks on device requires the game's real VM-thread and epoch ownership rules.

After acquisition, the adapter keeps only the live registration rows and their menu, event and callback names. It frees the input image. On Linux x86-64, a synthetic companion containing an unrelated 8 MiB payload and 1,000 duplicate subscriptions resulted in one active row occupying 198 bytes of adapter allocations. This count excludes the allocator's overhead and any VM-owned state. An injected allocation failure while copying the names released all acquired handles and published no registration set.

An independent i386 Windows check included the original Classic 1.7.3 `PluginAPI.h`. The serialization interface is ID 3, version 4, size 52, alignment 4. Its version and twelve function pointers occupy successive four-byte slots. Both event and form-delete callbacks use cdecl.

| Offset | Serialization member |
| --- | --- |
| 0 | `version` |
| 4, 8, 12, 16, 20 | `SetUniqueID`, `SetRevertCallback`, `SetSaveCallback`, `SetLoadCallback`, `SetFormDeleteCallback` |
| 24, 28, 32 | `WriteRecord`, `OpenRecord`, `WriteRecordData` |
| 36, 40, 44, 48 | `GetNextRecordInfo`, `ReadRecordData`, `ResolveHandle`, `ResolveFormId` |

The Windows test ran the original save, load and revert wrapper functions with an authored VM and storage. Classic invoked its serialization callback after the underlying VM save and load routines even when those routines returned false. The experiment activates registrations only after independently confirmed success. The Windows loader acquired eight referenced registrations from the original 4,353-byte fixture. Two successful explicit unregister operations released two references. VM revert then ran before core revert, followed by delayed-work cleanup, persistent-object cleanup, and the registered plugin revert callbacks in handle order. The registration maps and sets performed zero additional handle releases after the VM reset.

## Reading a stored Papyrus ModEvent

[object_history.h](object_history.h) reads Classic's `OBMG` storage header and version 1 `PapyrusModEvent` objects from the same accepted core snapshot. It checks the storage slot, owning stack ID, event name and typed argument list. The inspector reports integer and floating-point bit patterns, booleans, form IDs and their resolution status. A `None` argument has no payload. A missing or renamed plugin leaves its form unresolved. The raw stack ID and numeric form resolution require separate checks against a live VM before use.

The `PushString` argument has an important limit in this particular Classic format. The Win32 writer serializes the process-local string pointer in four bytes. Its file contains no corresponding string contents. The inspector consumes those four bytes, reports a process-local string, and returns zero for its value. The ModEvent name and registration callback names have their own length-prefixed storage and remain readable across restarts.

Unknown object classes, versions and argument types return distinct outcomes before any observer receives partial results. Their original bytes stay in the generation store. The experiment does not run object factories, instantiate Papyrus objects or resume delayed operations. One bounded pass validates at most 65,536 storage slots and 100,000 arguments. Subscribers that request field details receive a second pass over the same immutable image.

### Pending menu callbacks

`skse_ui_callback_objects` inspects an object store containing pending `UIInvokeDelegate` callbacks and `PapyrusModEvent` objects together. Classic's `UICallback.Create` places the delegate in persistent storage until `Send` or `Release`. The name of its target menu, the ActionScript target, the owning Papyrus stack ID, booleans and the exact 64-bit numeric arguments are present in the file. An ordinary `UI.Invoke` or `UI.InvokeForm` directly queues its command with the UI manager. The scanner covers the object-storage case.

The Classic i386 writer stores a four-byte process-local pointer for a Scaleform string argument. Its source reader expects length-prefixed string contents. A private test confirmed the four-byte word and the absence of the authored string text in a 4,132-byte Windows file. The inspector marks this argument as unavailable and returns zero bits for it. A separate numeric fixture, 4,124 bytes long, contained two pending UI calls and one ModEvent. The native inspector and original Windows loader agreed on all six UI rows. Adding an unrelated owner preserved the 4,104 core bytes, and Windows read back the same six rows.

### Stored forms and pending spawn tasks

The full storage reader in [object_catalog.h](object_catalog.h) recognizes all four factories registered by Classic 1.7.3. In addition to ModEvents and UI callbacks, `UIInvokeFormDelegate` contains the menu, method, VM form type and saved 64-bit handle. When the owning mod is missing, the Classic loader leaves the saved handle in that delegate. The inspector explicitly reports failed resolution and gives the caller the original handle separately. A factory's registration alone does not establish that a particular in-game menu saved one of its objects.

`SpawnerTaskFunctor` stores the Papyrus stack ID and an array of spawn requests. Each contains the spawn form, an engine reference handle, position and rotation, a count and two flags. Only the spawn form is resolved through the saved load order. Classic drops an entry whose form fails resolution. The target reference handle requires the game's live handle table. The inspector leaves this value opaque and preserves the exact single-precision position and rotation bits.

`skse_core_delayed_tasks` reads the four `DFMG` lists for queued, incoming, waiting and ready tasks. It retains the signed Windows performance-counter tick pattern on incoming and waiting entries. The file contains no timer frequency. A private 32-bit Windows test used Classic's original storage, UI form, spawner and queue save/load routines with authored forms and stacks. The 5,471-byte file held all four object classes, seven spawn tasks across storage and the queues, and one missing form in each task. The inspector agreed with every one of the Windows loader's 24 form, task and surviving-entry rows. Adding another owner preserved all 5,451 bytes of the core section, and the Windows reader reproduced the same rows. Tests also cover all sixteen empty/nonempty queue-group combinations and reject incomplete records before delivering any observation. Executing or scheduling these objects requires the live game VM, reference tables and timer policy.

## Crossing the save and load call boundary

[event_bridge.h](event_bridge.h) connects the generation store to a separately loaded, authored guest in the host test. At entry to its save-slot function, the guest calls the host to start a staging operation. Inside its save-VM routine, the host writes one synthetic owner's records to that stage. After the guest returns, the host validates call outcomes, nesting and an independent base-game result before publishing. The original `.ess` descriptor is duplicated at capture and held across any deferred publish. An empty set of participating owners produces no generation.

| What the host needs | What the test actually supplies |
| --- | --- |
| A particular call in this process | A launch nonce and the observed outer ticket. The passive recorder preserves the original seven call types, same-thread parents and guest return values. A missing return, an unexpected child, a reload-revert in place of a load-revert, or a dropped trace blocks the handoff. |
| Save completion | A successful nested `SaveVM`, a returned outer save, and a separate coordinator's `SKSE_BASE_SAVED` for the same event and read-only file. An absent coordinator yields `SKSE_BRIDGE_NEEDS_BASE`. A confirmed failure permanently rejects that pending save. |
| Load acceptance | A successful `LoadSlot` and `LoadVM`, a completed load-revert, and a coordinator's `SKSE_BASE_LOADED` with the actual descriptor consumed. The receiver is reversible, caller-owned scratch state. A rejected load leaves it empty. |
| The same save after a restart | A recovered profile, slot and persistent generation ID plus the file digest. The next guest uses a new process nonce and ticket. A reused basename carries no authority. |
| Profile and language | The running profile and effective text and voice language at launch. The experiment keeps each language combination in its own private store, and each row has a profile ID. Selecting another profile in Settings for the next launch cannot retag an in-flight operation. Cross-language replay is a separate integration policy. |

The source anchor for the running profile is `activeProfile` in `ios/runtime-port/mod_manager.m`. Effective `text` and `voices` come from `SkyrimGameLanguage` after `skyrim_game_language_plan_profile`. The app's interface language and requested language can differ from those values. `save_write_probe.h` measures file operations on candidate save names, so it supplies timing only. The authored coordinator in `fixtures/guest.c` supplies the independent final result and exact descriptor. The game-side source for that result, persistent generation identity, and lifetime of the exact file across replacement still need observation on a real device.

### Binding the input before the guest reads it

At an identified load's first open, `skse_bridge_prepare_load_input` produces a private, immutable read-only file for the loader. Its cursor begins at byte zero independently of the source cursor. The input has no pathname after capture. The loader can close its descriptor after the game has finished reading. Its coordinator needs its own handle to the file the game actually used.

`skse_bridge_finish_load_input` consumes one pending request. It checks the slot return, VM return, load-revert, coordinator outcome, request identity and identity of the immutable input. The generation store then verifies the complete base digest before returning a private copy of the companion. The owner can inspect that copy. Activating state in a world also requires the VM owner's current epoch and registration or plugin lifetime guarantees. Cancellation uses `skse_bridge_drop_load_input`.

The test loader reads a genuinely sealed file on Linux. A separate filesystem test double exercises the Apple clone's open, replacement and unlink sequence. Reopening an identical quicksave under the same numeric descriptor fails before any hashing. A failed game or VM return, missing generation, mismatched base digest or incomplete trace returns zero companion bytes. The app's save-check screen continues to inspect the original files read-only. Adopting this experimental input for gameplay requires confirming the real loader can consume a supplied descriptor for every read, recovering the same durable save ID, and observing the owning game's and VM's terminal outcomes.

### Moving the final save hash off the calling thread

`skse_bridge_defer_save` checks the same completion evidence and takes an immutable snapshot of the exact open file. The worker opens its own connection to the same protected store and calls `skse_bridge_publish_deferred`. The first attempt hashes the snapshot once. Retries reuse that digest. Changing the original file in place or replacing its pathname leaves the captured generation available to the worker. Loading still verifies the file that the game actually consumed. Supplying an existing base to `skse_bridge_begin_save` still verifies that base during staging.

On Apple the experiment uses `fclonefileat` in a caller-owned private work directory on the source volume, opens the new inode read-only and immediately unlinks it. An unsupported volume returns an explicit result. The caller chooses whether and when to use the synchronous path. A process exit immediately after cloning can leave `base.clone` in that one work directory. Retrying in it returns a conflict and preserves its contents. Its owner can discard the abandoned work directory. The recovery path only uses published database generations. [Apple clonefile manual](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/clonefile.2)

On Linux, the experiment accepts an already write-sealed anonymous file or a filesystem-supported `FICLONE` into an anonymous temporary inode. A host test also exercises the Apple-style name lifecycle with an explicitly substituted clone operation. The local WSL filesystem reported clone unsupported for an ordinary file. APFS behavior and latency need a run on Apple hardware.

| Synthetic base file | Original synchronous pin | Original verified publish | Capture of a pre-sealed Linux file | Publish on a separate worker |
| --- | ---: | ---: | ---: | ---: |
| 1 MiB | 2.458 ms | 7.633 ms | 0.026 ms | 9.799 ms |
| 16 MiB | 39.598 ms | 44.835 ms | 0.022 ms | 45.540 ms |
| 64 MiB | 158.690 ms | 163.373 ms | 0.023 ms | 175.702 ms |

These are cached WSL host timings from five measured runs after one warmup per size, with a 2 KiB synthetic owner record. The Linux capture column begins with an already sealed descriptor. Creating or filling that file is outside the timed interval. The two columns on the right measure the host handoff and worker separately. The native tests also count full-file hashes, with zero during capture, one on initial publication and zero extra on retry.

The `observer` directory contains the project's unchanged passive recorder from its Classic lifecycle experiment. It retains the first 256 rows in a process. The test accepts completed, unambiguous same-thread call trees. Live events routed across threads, a rewritten call graph or a full trace need a separately verified event channel. The guest, its test binding and the bridge have no build connection to the app.

## Tests

The Linux test requires SQLite and OpenSSL development headers and libraries. On macOS it uses system SQLite and CommonCrypto. The adapter bounds each `.ess` scan to 512 MiB, each companion to 64 MiB and the combined owner/record headers to 100,000. These are experimental resource budgets. Each edit currently constructs a new in-memory companion. The database retains published generations and needs a separately validated retention policy before long-term gameplay.

From `pc-aot`, run the authored suite with the following command.

```sh
python3 -B -m unittest discover -s tests -p 'test_skse_cosave*.py' -v
```

Tests cover name reuse with the same timestamp and size, identical contents from different events, overwritten data between pin and commit, unknown owners, all-or-nothing multiple edits, missing owners, partial receiver rollback, malformed files, limits, row corruption, concurrent publishers and interruption before and after publication. The crash fixture flushes dirty SQLite pages and exits with a real rollback journal present. The restored database must contain either the previous state or the complete new generation.

The byte-level reader and editor derive from the project's reviewed Classic implementation at `5fffebaf7b4429034412c3ecd6d87b5a1667d0de`. A separate private Win32 fixture compiled the original Classic 1.7.3 header structures and serialization routines against authored file I/O. Its writer produced a 1,111-byte companion. After changing one owner and adding another, the adapter exported 1,149 bytes. All 1,049 bytes of the unrelated owner section matched the source. The rebuilt Win32 reader verified record type, version, size and payload checksum with all three owners registered and with two different subsets registered.

A second private Win32 fixture compiled the original `SaveModList`, `LoadModList`, `ResolveModIndex`, `LoadedModFinder`, `GetModIndex`, `ResolveFormId` and `ResolveHandle` definitions against an authored DataHandler. Its core wrote 255 names. After reversing load order and removing one plugin, the native map matched all 512 success flags and output values across 256 form IDs and 256 handles. The Win32 reader returned the same results after the adapter added an unrelated owner's record. The fixture supplied indices and file I/O. It did not run the game or its VM.

A third private Win32 fixture used the Classic menu registration writer and reader with three authored menu names and five handles. Both readers produced the same four remapped 64-bit values and skipped the handle of the missing plugin. The Win32 fixture repeated the check after an unrelated owner was appended and counted four calls to its synthetic VM handle-reference service. The native reader only reports the observed rows.

The ModEvent fixture verified three authored event names, five saved registrations and four resolved event, callback and handle triples, including a 256-byte callback name. A final Win32 fixture ran the original `Core_SaveCallback` and `Core_LoadCallback` with all eight registration families and inert object-storage stand-ins. Its 4,353-byte file held 12 registrations. The native inspector resolved nine numeric handles and identified three removed-plugin entries. The official reader loaded eight of those nine, omitting the `NINU` entry it had written. Changing only the case of that tag in a separate synthetic file made the official reader load the ninth. Adding another owner preserved all 4,333 core section bytes and left the eight Windows-loaded registrations identical.

A separate 32-bit Windows fixture compiled the original six ModEvent, persistent-storage and object-helper save/load functions using authored forms, a test factory and stack state. Its two events occupied two of six slots and contained eight arguments. The inspector and the native Windows reader agreed on all 11 summary, event and argument rows. The source process checked that the string argument occupied one pointer word and that its text was absent from the file. Another form's owning mod was missing. Replacing an unrelated owner preserved all 4,037 bytes of the core section, and the Windows reader produced the same 11 rows. No captured pointer was dereferenced on load.

The boundary suite loads its own guest through `dlopen` with a versioned binding. It exercises nested callbacks, two overlapping OS threads, mismatched proofs, independent effective languages and profiles, a fresh launch, missing or malformed events and termination after the base save, before the database commit and immediately after it. Fixture control supplies the final base outcome independently of the instrumented call result. All `.ess` and `.skse` inputs are authored test files opened read-only by the bridge.

## Requirements for game integration

1. Observe actual save completion and load acceptance or failure for manual, quick and automatic saves. Supply an authoritative event ID, a stable source descriptor and a way to recover that same ID on load after app restart. The current game observer identifies candidate entry/return boundaries. File writes, names, timestamps and keyboard events cannot provide this contract. Simultaneous saves, renames, backups and game deletion also need verified ownership and ordering.
2. Define what happens if the game exits after committing an `.ess` but before publishing the companion. Hold immutable input while hashing, or coordinate the writer and reader. Measure hashing, peak memory, save time and database growth on device before connecting it to a game thread.
3. Supply the live DataHandler catalog when restoring the verified core-owner mod history. The remaining UID 0 data in official `InternalSerialization.cpp` includes event registrations, persistent objects and delayed operations. Its revert callback clears registrations and releases owned objects. Copying UID 0 bytes and resolving numeric references preserve information for a compatible reader. Reconstructing VM registrations requires separately verified game calls.
4. Connect trusted plugin registration and unique UID ownership to real save, load, revert and form-delete callback lifetimes. The host experiment supports record streaming, empty-owner load callbacks and form and handle resolution. It currently invokes caller-owned test functions. General Windows DLL entry points, live VM objects, form deletion notifications and rollback of plugin-owned state need independent evidence.

The existing Classic lifecycle observer can capture nested slot, VM save, VM load and revert calls by numeric ticket. The outer `BGSSaveLoadManager::SaveGame_Hook` has a void return. In `Hooks_Papyrus.cpp` the save and load wrappers call serialization after the original VM routine and return that routine's boolean. Successful VM work alone does not establish that a final base-game file has reached disk. The next bridge needs the original completed base-file handle and a recoverable slot binding, official VM revert ordering and the outer load result. It can pass these to this adapter once the game's threading and file lifetime rules have been demonstrated.
