# Runtime observation of fresh save publication

This hosted experiment stages a private copy of `vendor/skydream/runtime/kernel32.c` at the pinned Skydream revision. The optional prepared mode uses the existing iOS kernel preparation before applying the observer. Its file calls execute the real runtime path mapper, handle table, and POSIX operations against authored data in a temporary directory. The normal app build has no reference to this experiment.

| Observed boundary | Evidence used |
| --- | --- |
| `CreateFileA` under the fresh factory/check scope | The caller must have the verified CRT return token `0xF6BF24`. The real mapped output must be the authorized `.ess.tmp`. The observer opens a read-only descriptor while the writer is live, checks its inode and assigns the host file object a per-open token before publishing the guest handle. |
| `WriteFile` or `WriteFileEx` | The observer copies the descriptor and token before I/O and records the signed syscall return and requested length. A short positive result is a write failure for the witness. The guest count, return and APC bookkeeping retain their original meanings. |
| BSFile flush and `FlushFileBuffers` | The lifted `f_B29850` return provides the stream flush result. `FlushFileBuffers` in this runtime returns true without any host flush, so its observation is recorded independently and provides no durability evidence. |
| `CloseHandle` and CRT close | The runtime exposes the actual POSIX close status before the original shim frees the file object. The caller must match `0xF67FC8`. A successful witness also requires the lifted CRT close result and the matching file version at closure. |
| `MoveFileA` under the final move scope | The lifted `0x680317` call authorizes the final rename. With CRT return token `0xF53F17`, the runtime checks both mapped names, the source identity and the raw rename result. The retained reader and final directory entry must agree afterward. Backup rotation runs through the same host wrapper outside the final scope. |

The wrapper's guest Boolean is insufficient in three concrete cases. A short `WriteFile` can return true, `WriteFileEx` can return true while its completion reports an error, and `CloseHandle` returns true after a failed POSIX close. `MoveFileA` leaves the guest last-error value untouched on failure, so the lifted CRT wrapper can translate that failure to zero. The fixture exercises all four conditions with the runtime's original guest behavior.

## Hosted verification

From `pc-aot` on Linux or WSL, run the base fixture and the optional composition with a completed original-body generation.

```sh
python3 -B experiments/classic-runtime-save/test_runtime_save.py --sanitizers

python3 -B experiments/classic-runtime-save/test_runtime_save.py --sanitizers --prepared \
  --generation /path/to/private-generation
```

The direct test covers real short `write` and `pwrite` results with file limits, a failing `pwrite`, separate stream-flush and stub results, actual close failure, an unscoped call, a failed or replaced rename, a cross-thread tagged write, and a late event from the previous observation. The composed test reuses the verified lifted save tail, BSFile destructor, save VM caller and CRT move wrapper. Its authored engine services route the file operations through the same staged runtime and compare the retained reader with the independently checked publication witness. Every compiled unit uses the same pinned CPU headers when lifted code is present.

## Integration conditions

The observer tracks one owned fresh save at a time. A tagged operation from another thread invalidates that observation without dereferencing the owner's state. A late operation from an old token leaves the newer observation alone. The inherited handle table still requires normal guest handle ownership. The observer cannot repair a race between file lookup and close. The active save/profile must supply the authorized directory and per-attempt lifetime explicitly, including cleanup on nonlocal exits and before shutdown. Concurrent writers using independent handles require separate coverage and serialization evidence.

The staged hot paths snapshot primitive values before blocking operations. They take no new locks around the syscalls and call the observer on writes only for a tagged file. The reader and file identity checks are limited to the owned save. Before dispatch to a background worker, the caller still needs an immutable snapshot and an authenticated event handoff. The experiment leaves actual iOS scheduling, APFS capture, cached streams and physical durability for separate verification.
