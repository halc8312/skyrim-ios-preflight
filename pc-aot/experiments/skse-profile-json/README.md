# Profile JSON storage experiment

A host grants one profile identifier, one exact producer digest and one virtual directory prefix. The store duplicates an existing app-private directory descriptor. Each identity gets its own namespace. Guest paths are normalized before the file service receives them. The store rejects traversal, accepts portable ASCII names with Windows-style case folding, and admits only JSON files beneath the granted prefix. An unambiguous existing alias keeps its spelling. Two aliases fail even when one matches exactly. Links, devices and detached directories fail validation. The experiment takes the app-private descriptor from its caller, so the caller must open an isolated store directory. All tests use disposable scratch data.

## Virtual files and publication

- The base asset provider remains immutable. A definite base hit wins over the profile input. An unknown base namespace prevents profile writes, while a confirmed miss allows the grant to supply the bytes. Ungranted writable names retain the session-local behavior of the file service.
- Opening a saved value for write promotes the immutable snapshot into a bounded session copy. Existing compatible readers share the copy. Opening without a mutation does not stage a write. A failed write or reported failed guest callback blocks capture. A dirty output must have no remaining handles to that output. Unrelated reads and logs can remain open.
- The host copies every eligible output before publishing any. A staging failure returns an empty batch. Each copied snapshot belongs to the original open store lease. A reopened store can read committed values for the same identity. A snapshot from an earlier lease cannot write there. The app must keep an overlay and its store alive while the VM uses them and serialize guest callbacks and staging for that VM. Overlapping read-modify-write workflows for the same identity would need app-level coordination.
- File data is stored as opaque bytes. Grant caps limit the size of a file, committed bytes and file count for that profile and producer. The store also bounds directory depth, directory count and directory scanning. Each commit checks the quota under the process-wide storage lock.

## Interruption and recovery

The store syncs the private staging file, verifies the destination and pinned parent again, then replaces the published file with one same-directory rename. An error before that rename preserves the prior file. Process termination before rename can leave one reserved staging file in the directory. A later scoped commit can replace a stale staging file only if its name and private regular-file attributes match. Reads ignore it. A directory-sync failure after rename yields a separate result because the new bytes may already be visible. The snapshot still counts as published. The host must inspect the saved state before retrying. File and directory syncs do not establish a physical power-loss guarantee on iOS. [Apple file sync documentation](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html)

The store assumes one app process controls the private directory. The tests cover a directory detached before publication. They exercise pointer and descriptor lifetimes without granting the guest access to host paths.

## Verify on a host

Run `python3 -B -m unittest -v test_profile_json` in this directory. On Linux, set `SKYRIM_TEST_SANITIZERS=1` for ASan and UBSan. The Linux test runs multiple virtual sessions and injects a short write, storage errors, a process exit before rename and a detached directory. The native Windows test compares the portable virtual overlay to authored files opened with all five `CreateFileW` dispositions and with shared handles. The storage backend uses POSIX relative descriptors and has no native Windows backend. [Win32 file-open semantics](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)

The app does not use this store yet. iPhone validation still needs the app-owned directory, profile switch, actual MOD callback and restore path.
