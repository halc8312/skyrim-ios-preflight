# Classic Safety Load 1.2 allocation branch

The author's documentation describes skipping a reclamation step in the allocator. The owned 1.2 release, with SHA-256 `5a6eb517ed331892f43f9f4516e89781a39150fe5f9564f1f9143324884892a9`, was inspected read-only. Its initialization proposes a branch rewriting the allocation path in the fixed LE 1.9.32 image. It calls the original reclamation path for nonzero mode, when RaceSex Menu is open, or when its configured loading-only mode excludes the current interval. For small mode-zero allocations when allowed, it clears the local mode and resumes at the existing plain or aligned allocation fallback. The release DLL also has a separate branch for unusually large requests. [Safety Load 1.2 by kapaer](https://www.nexusmods.com/skyrim/mods/46465)

The staged experiment checks the original unit hash and allocation region hash. It modifies one branch in the generated private copy and uses the original plain or aligned fallback. The policy callback is supplied by the hosted test. The experiment limits bypasses to positive requests below 256 MiB, mode zero, and a confirmed loading interval outside character creation. Other requests retain the original path. The original allocator loops after a failed allocation. The experiment permits one bypass per call so a failed fallback re-enters the ordinary reclamation path on the retry.

The test compiles the complete staged unit and compares CPU registers, flags, stack layout and service calls across the original and staged versions of the branch. It covers disabled, loading closed, character creation, nonzero mode, zero size, above the supported size range, and both fallback allocators. It drives the original retry branch with an authored failure from each fallback. The allocator services and the upstream retry reentry in this fixture are stand-ins. App integration needs authentic menu state, runtime allocator behavior and an iPhone comparison of loading duration and memory pressure. This experiment leaves the app unchanged.

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_allocator
```
