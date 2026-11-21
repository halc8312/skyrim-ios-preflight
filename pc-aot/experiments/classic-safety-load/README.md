# Classic Safety Load 1.2 allocation branch

The author's documentation describes skipping a reclamation step in the allocator. The owned 1.2 release, with SHA-256 `5a6eb517ed331892f43f9f4516e89781a39150fe5f9564f1f9143324884892a9`, was inspected read-only. Its initialization proposes a branch rewriting the allocation path in the fixed LE 1.9.32 image. It calls the original reclamation path for nonzero mode, when RaceSex Menu is open, or when its configured loading-only mode excludes the current interval. For small mode-zero allocations when allowed, it clears the local mode and resumes at the existing plain or aligned allocation fallback. The release DLL also has a separate branch for unusually large requests. [Safety Load 1.2 by kapaer](https://www.nexusmods.com/skyrim/mods/46465)

The staged experiment checks the original unit hash and allocation region hash. It modifies one branch in the generated private copy and uses the original plain or aligned fallback. The policy callback is supplied by the hosted test. The experiment limits bypasses to positive requests below 256 MiB, mode zero, and a confirmed loading interval outside character creation. Other requests retain the original path. The original allocator loops after a failed allocation. The experiment permits one bypass per call so a failed fallback re-enters the ordinary reclamation path on the retry.

The branch fixture compiles the complete staged unit and compares CPU registers, flags, stack layout and service calls. It drives the original retry branch with an authored failure from each fallback. A second fixture executes the pinned plain and aligned CRT routines, with a HeapAlloc adapter into the original Skydream guest heap. It verifies the aligned allocation's owner metadata and a null return with ENOMEM when the adapter injects failure.

The private guest can include `safety_load` together with `loading_menu_observe`. The host selects both stages only when the profile opts in, the effective DLL matches the verified author release, and the pinned guest is ready. The app uses the verified loading and character-creation transitions even when detailed logs are off. While loading, it also checks the app's current memory-warning window. This trial covers the loading-only policy. End-to-end iPhone timing, stability, and memory usage still need comparison.

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_allocator
```
