# Hosted `.ess` and `.skse` roundtrip

One runner drives a save process and a fresh load process against the same temporary profile and generation store. The save process runs the pinned lifted save and destructor call sites with the actual iOS-prepared Skydream file wrappers. The load process runs the pinned selected-save, VM revert and VM load call sites with authored inner services. All save bytes and the single Classic owner record are authored for this fixture.

The save callback retains the runtime observer's exact published read-only descriptor and its metadata. After leaving the fixture's game scope, it binds that descriptor to the final directory entry and an event key for the exact file version. The bridge checks the named version again around immutable capture. The original bridge is closed before a separate thread opens its own store connection and publishes the staged companion. A different launch scope is rejected before hashing. Retrying the successful publication reuses the cached digest.

The loader opens the published `.ess` at the reviewed fresh-open site and derives the key independently before capturing the selected file. The load bridge hands it a distinct sealed read-only descriptor at offset zero. It reads the authored preflight byte and the first slot byte from that descriptor, verifies the recorded VM revert and true VM return, and checks the same descriptor during completion. On success, it verifies the full base-file digest and the exported Classic `.skse` image and owner record. The key is for the saved file in its profile on the same filesystem. Copies, edits and replaced directory entries require separate import or pairing.

Each attempt opens an owner-thread lifecycle scope before the first observed call and closes it before handing storage work to a worker. The scope has its own bounded trace and can record a new attempt after the process-wide diagnostic prefix fills. The save fixture exhausts that prefix, records 256 subsequent save attempts, and checks that delayed returns from a cancelled attempt cannot complete a later attempt with the same local ticket. A full or unfinished attempt is rejected.

| Case | Expected persisted result |
| --- | --- |
| Published save and successful VM save | Fresh load returns the exact owner record. |
| VM save false or actual short write | The base file can exist, and the companion is suppressed. |
| Owner record staging fails or immutable capture is unavailable | The staged companion is discarded without hashing. |
| Deferred job is interrupted before publication | The base file remains loadable and no companion is produced. |
| Same-byte file replaces the published inode before capture | Identity check suppresses the companion before any capture. |
| File contents are replaced after capture and before worker publish | The worker retains its original sealed snapshot. The later load sees the replacement and rejects that stored generation. |
| Existing stream at `manager + 0x30` | Cached load succeeds without opening a new bridge input or restoring a companion. |

## Run on Linux or WSL

From `pc-aot`, run with a completed private original-body generation and the SQLite and OpenSSL development files. Use `--cflag` and `--ldflag` for nonstandard development paths.

```sh
python3 -B experiments/classic-save-roundtrip/test_roundtrip.py \
  --generation /path/to/private-generation --sanitizers
```

Add `--project-actor` to verify a copied actor from the original Classic FormID lookup after the approved companion load. It passes that copied identity and the accepted fixture epoch to the original managed Papyrus argument path. This uses the same completed lifecycle, immutable base input and persisted generation.

The runner stages the two clone syscalls in the Linux immutable capture object. For this fixture they copy the exact verified source into a `memfd` and apply real kernel write, truncate and grow seals. The store, bridge, descriptor checks and record format are the existing implementations. The fixture checks lock availability at each emulated clone read and digest read, and requires the saver to hash only once, on its worker, even after retry. The Linux clone substitution performs a copy outside the game scope and supplies no APFS latency evidence.

Each host phase admits one owned event. The existing file handle lifetime rules still apply. Actual device scheduling, on-device immutable capture, and pre-existing-stream provenance require separate validation. The app and original game files are outside the fixture.
