# Classic package and idle event fixtures

Each hosted fixture verifies its generated unit against the reviewed SHA-256 and stages a single matching instruction site in temporary private storage. The engine-side form checks use the SKSE 1.7.3 `TESForm` layout. Subscribers receive copied IDs and bounded text. The fixtures link compiled callbacks directly.

| Site | Hosted acceptance |
| --- | --- |
| `decision`, `0x6A9E90` | After the original package call and `RELOAD()`, the actor in ESI and original package in EAX provide two checked IDs. Only a lookup returning the matching `TESPackage` can replace EAX. |
| `change`, `0x8CAB54` | The `OnPackageChange` block delivers a pair of checked FormIDs before the original string assignment and engine dispatch. Its `OnPackageStart` and `OnPackageEnd` sibling branches keep their original order and skip the notification. |
| `idle`, `0x6F02FF` | An exclusively owned `TESActionData` request exposes a checked reference and up to 127 event bytes. An Idle with matching ID is applied through the lifted `StringCache::Ref` assignment. Both original flag branches still run in place. |

The package change packet at EDI carries an actor pointer followed by a package FormID. The original engine takes a reference through the first word and looks up the second. PapyrusUtil LE 3.3 passes the first word directly to `LookupFormByID`. The fixture derives the actor FormID from the live object and checks that lookup returns the same object. This agrees with the author's later hook, which reads the actor's `formID` before performing cleanup. [PapyrusUtil package-end source](https://github.com/eeveelo/PapyrusUtil/blob/master/PackageData.cpp#L246-L257)

The idle fixture checks the `TESActionData` vtable, borrows the request for the duration of the callback and applies its new Idle pointer and event ref together. A missing borrow, stale reference, failed choice or invalid candidate preserves both fields. The fixture exercises the exact lifted string assignment with synthetic retain and release services. Live engine ownership, queue interactions and final iPhone behavior require separate validation.

Run with a POSIX host compiler and the reviewed private original-body generation:

```sh
python3 -B experiments/classic-package-decision/test_package_decision.py \
  --generation /path/to/private/original-body-generation --sanitizers
```

Run the same command with `--site change` or `--site idle` for the other two cases. Each test runs its verified lifted code against authored engine services. Synthetic forms and code pages stay read-only during execution. Only these hosted fixtures link the callbacks. Live form lifetime, actor-specific package eligibility and PapyrusUtil activation remain unverified.
