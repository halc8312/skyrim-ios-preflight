# Classic favorites query

SkyUI 5.1 LE lets players bind a favorites group in its Mod Configuration Menu. Both the bound key and the favorites menu can activate the group. When equipping it, SkyUI calls `Game.IsObjectFavorited(Form)` for the saved left-hand item, right-hand item, and other group members. A negative answer rejects that item. The initial Journal menu uses separate event registrations and `UI.InvokeStringA`. The shipped 5.1 scripts were inspected offline. All experiment inputs are synthetic. [SkyUI 5.1 LE](https://www.nexusmods.com/skyrim/mods/3863)

The prepared build 18 registry contains 100 entries and none for this function. A locally built Windows x86 comparison runs the original 1.7.3 native function, including its searches through the magic and inventory collections. The independent C candidate reads the same objects through a checked 32-bit memory interface. The fixture supplies the player, sorted favorites, inventory instances and underlying RTTI service. [Classic SKSE source distribution](https://skse.silverlock.org/)

## Running the comparison

On Windows with Visual Studio's x86 C++ tools and a private copy of SKSE 1.7.3 extracted from the official archive, run

```powershell
py -3 compare.py --official C:\private\skse_1_07_03
```

The script verifies the six SDK input files by SHA-256. It builds and runs both unoptimized and optimized references in a temporary directory. It needs Python, the SDK source, and the existing C/C++ compiler.

Each Windows build matched **16,403** checks, with **5,605 true** and **10,798 false**. Membership uses form object identity. Spells and shouts use the sorted magic favorites array, including favorites without a numbered slot. Ordinary items use the extra hotkey on any instance of the matching inventory entry. An existing extra with key `-1` counts. A presence bit without the matching object does not. Nonpositive inventory count deltas do not override a present extra.

Eight additional candidate checks cover damaged or partially initialized state. In particular, Classic's `FindHotkey(Form*)` leaves the result uninitialized when changes exist but their data or inventory-list pointer is missing. The candidate returns false for both. The Windows reference is never called for those cases. Recognized corrupt list cycles, array metadata and failed guest reads produce the distinct `FAVORITE_INVALID` result.

## Integrating with the title

1. Register the static Boolean function with an actual `Form` parameter. Obtain its type from the VM class registry as in `PapyrusArgs.cpp::GetTypeIDFromFormTypeID`, including release of the temporary class reference. The current raw argument tags occupy one byte, while a class descriptor is a guest pointer. Resolve that descriptor when populating this function's parameter slot. Use the regular scheduling flags from the Classic registration.
2. At dispatch, preserve the source `VMValue` address for the parameter. Follow the identifier's lock-protected handle, the policy's type and validity checks, and `Resolve`. Pass the resulting guest form to `classic_favorite_query`. A null or expired form yields false. The experimental input-event receiver already models this handle lookup. Creating a new registration handle belongs to the event receiver only.
3. Supply the current player, magic manager and an engine-equivalent `BaseExtraList::GetByType` for the container and hotkey types. The latter checks the presence bit and the underlying extra type. The callback in this experiment uses the original Classic lookup with MSVC RTTI for the test objects. Keep live inventory reads on the engine's normal scheduled path. The other favorite-group operations, including actor equipment changes, need their own checks.

The read-only core also compiles to an arm64 iOS 18 object. Integration with the game's VM, live inventory access and execution on an iPhone remain unverified. The public experiment contains no game or MOD files.
