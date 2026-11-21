# Classic actor FormID projection

The hosted fixture uses the original `Game.GetForm` caller, the original FormID table lookup and the original lock acquisition and release. All form objects and thread services in the fixture are authored. The test returns one copied actor FormID and its form type after the same scoped load is confirmed. It checks the existing co-save lifecycle trace and preserves the native lookup's original stack and return value.

## Checked engine boundary

| Pinned LE site | Observed contract |
| --- | --- |
| `0x8F0C15 → f_451A30` | The vanilla `Game.GetForm` body `f_8F0C10` reads the fourth stack argument and calls the cdecl FormID lookup with return token `0x8F0C1A`. SKSE 1.7.3 declares the same lookup in `GameForms.cpp`. |
| `0x451A38 → f_A4AEE0` | Lookup acquires the registry read guard at `0x12E59E4` and traverses the FormID table at `0x12E59D4`. |
| `L_451A93` | A matching bucket yields its borrowed object address. Before unlocking, the fixture copies the ID at `+0x0c` and checks the type at `+0x12` through the existing bounded form view. The copied ID must equal the requested ID, and the type must be `0x3e` (Character). |
| `0x451A9B → f_A4B0F0` | The original hit releases the read guard, then returns the borrowed address in EAX without ownership. A matching form erase in `f_451D50` takes the same registry's write guard. |
| `0x8B3EEB → f_451A30` | A separate branch in the Classic VM handle policy's `Resolve` also checks the returned form's type byte for `0x3e`. The staged test uses the simpler, approved `Game.GetForm` caller. |

For a live handoff, the engine owner needs to establish the form lifetime through the copy and the current VM's acceptance of the exact load ticket and saved generation. An actor's deleting destructor `f_6CE610` runs actor and reference teardown before the base destructor `f_452250` calls the registry unlink helper `f_451BD0`. A lookup can therefore see a mapping while its object is already in teardown. The registry lock alone cannot provide a live actor or justify acquiring a reference from a borrowed address. A verified owner phase or an already retained native reference is required. A FormID is separate from a reference handle and a VM handle. Keep the published result to the copied ID and type, and revalidate it for later uses.

The lock section does one bounded memory copy. It has no plugin callback, handle acquisition, storage access, or trace snapshot. The host validates the load before calling and checks the accepted epoch after unlocking. The fixture removes host access to the form at unlock to verify the result survives without a later dereference. Other cases reject a miss, unrelated caller, invalid address, wrong FormID or type, incomplete or failed load, and stale epoch, ticket or generation.

## Run

Run the standalone check with a POSIX host compiler and reviewed private original-body generation.

```sh
python3 -B experiments/classic-form-projection/test_form_projection.py \
  --generation /path/to/private-generation --sanitizers
```

The same check can run after the existing persisted save roundtrip. Add `--project-actor` to its documented command. It receives the actual load event after `skse_bridge_finish_load_input` has checked the immutable base input, original scoped load and persisted generation. Missing and replaced companions do not admit a lookup. The cached route has no handoff. After a matched result the same authored actor ID, type, and accepted fixture epoch enter the managed-argument check below.

## Managed Papyrus argument

The pinned game calls `f_8F9720` at `0x8D83CB` during VM setup. The port's `skse_entry.c` publishes the existing SKSE natives after the original registration routine on that registry. This registration precedes any later selected-slot load and supplies no evidence of its accepted VM epoch.

The original Actor registry entry uses `f_8DECB0` to construct `GetRelationshipRank` with callback `f_8DB8C0`. Its vtable stores native `Invoke` in slot 15 and typed `Run` in slot 22. The fixture exercises the following pinned path.

| Site | Use in the fixture |
| --- | --- |
| `0xC46E5F` | `f_C46CB0` calls slot 22 on the non-latent branch, passing the copied base value, registry, stack ID, result value and VM state. |
| `0x8E5B9C` and `0x8E5BB1` | `f_8E5B90` finds the current argument page and argument zero using the original `VMArgList` functions. |
| `0x8E5BB8 → f_8D9510` | The native actor decoder checks the identifier and uses the engine handle policy's type, availability and typed resolve calls. The borrowed actor in EAX continues through the original callback. The fixture's observer takes a copied ID and type only under a separately provided actor lease. |
| `0xC46E78 → f_C329E0` | Original direct completion publishes the native result in the VM stack and clears the temporary values. The fixture checks the same integer output and stack discipline with accepted and refused snapshots. |

The verified `VMIdentifier::GetHandle` lift `f_C307C0` borrows the identifier lock at `+0x18` while copying its two handle words. The separate identifier retain/release transitions call the engine handle policy. Their native entries `f_8B5040` and `f_8B51A0` update handle bookkeeping. This is insufficient evidence of a retained native actor. The resolver can use the FormID table that permits an actor already in teardown. Even the player singleton's stock replacement path releases the old handle refcount before writing its new pointer at `L_698F99`. A live bridge needs an engine-owned argument frame and reference scope through the field copy, and the same accepted load epoch before the result is handed off. It cannot reuse a registry pointer, frame address or handle from a prior load.

Run the managed check independently with the same command and `--managed-actor`. The argument page traversal, typed unpack, identifier handle copy, and native call order are pinned lifted code. The guest argument contents, policy responses, native query result, VMValue assignments and teardown services, and actor lease are authored. The test covers absent arguments and native ownership, invalid argument kind or type, replaced actor or registry, and retirement during the call. The fixture protects the actor page after the call so the copied value survives without another access. Neither mode activates a game mod or verifies live iPhone behavior.

## Retained engine reference

Classic SKSE 1.7.3 `Hooks_Gameplay.cpp` identifies the crosshair reference lookup at `f_739FD0 + 0x64`. The original game uses a 32-bit reference handle from its pick state, clears its local output slot, and calls `LookupREFRByHandle` at `0x73A034`. The slot is owned by that caller until its cleanup. The fixture observes only the successful return at `0x73A039`.

| Pinned site | Ownership evidence |
| --- | --- |
| `0x4A91BB → f_4A81B0` | `LookupREFRByHandle` assigns the ref-table object to the caller's slot. For the initially empty slot, `f_4A81B0` increments the `BSHandleRefObject` count at reference `+0x18`, before reporting success. |
| `0x4A91C0` through `0x4A924C` | The lookup checks the slot's occupancy and generation and the object's reference index. A rejected lookup clears the caller's slot and drops the provisional reference. |
| `f_43C400` | Table retirement holds the table write guard, invalidates the entry at the same index, and decrements the table's reference. The original routine leaves the handle argument's word alone. |
| `0x73A22E` and `0x73A242` | The crosshair caller decrements its local reference after use. It invokes `DeleteThis` if the low ten refcount bits reach zero, consistent with `GameReferences.h`. |

Run the standalone check with `--retained-actor`. The original caller, retain, validation, table retirement, and caller release are pinned lifted bodies. The host authors the guest actor/table fixture, imported atomic operations, unrelated UI services, and a final-destruction witness. It checks success, no observation, zero and stale handle, wrong index, inactive slot, wrong type or ID, and epoch retirement. It also runs the original table retirement between acquire and copy. The caller's reference keeps the object alive until the original cleanup, then the witness removes access to the actor page. The result contains copied fields only.

The managed 64-bit VM identifier and the 32-bit reference handle come from different owners. `IObjectHandlePolicy::Resolve` can read the raw FormID table through `f_451A30` and return its borrowed object without the crosshair caller's reference count increment. A matching FormID or pointer does not establish a reference handle. The crosshair proof applies to the already-successful original handle scope. It does not establish concurrent safety for a new lookup begun after borrowing a pointer, expose an arbitrary actor, or wire a live Papyrus call. A gameplay bridge still needs to pair its specific managed argument and accepted load with an independently proven native owner scope.
