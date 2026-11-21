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

## Selected file witness

The optional load-witness stage tags the real GFile published by the pinned 0xF6BF24 CreateFileA return. It requires GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, the selected stream's reviewed outer frame, and the exact mapped .ess from the caller-authorized profile directory. The read-only descriptor and named entry must match the version captured before the guest open and still agree at handle publication. The existing save token remains independent.

A known stream and native guest handle can then prove the same live generation. CloseHandle invalidates the generation before releasing the original descriptor. A recycled handle cannot prove an earlier generation, and a changed directory entry or replaced descriptor fails the check. The witness never reads the file bytes. The hosted CRT observer below can use this generation. Immutable snapshots, companion restore and app binding require additional work. The original handle table's caller-ownership requirement still applies.

With both `--load-witness` and `--generation`, the hosted fixture also checks the link from the selected stream to the runtime handle. For this fixed loose-file reader, `f_677990` uses the BSFile pointer at stream offset `0x104`. The `f_A57260` and `f_A572A0` callbacks dispatch virtual slot `0x44` to `f_A57E70`; the BSFile's CRT stream lives at `+0x24`. The CRT routine `f_F680D9` gets the descriptor at `FILE + 0x10`. Its low-level reader at `f_F6ABEE` checks the count at `0x1BC0320` and the open bit in a 64-byte descriptor entry. A 64-page table at `0x1BC0340` holds 32 entries per page. The entry's first word is the Win32 handle passed by the real read at `0xF6ADEB`.

After the selected open, the observer checks CRT stream registration, read-only flags, the descriptor bounds and the existing GFile generation. It repeats those checks on both sides of the pinned slot read and ties the result to its original receiver, 100-byte request, and actual return count. A changed CRT object, descriptor entry or native handle invalidates the result. The hosted test reuses the actual lifted reader and its BSFile buffer path. It fails if that path tries to refill the buffer and verifies that the same authored file remains unchanged. The test uses authored guest stream state and the prepared runtime. An on-device adapter, live engine ownership across threads and an immutable snapshot remain separate prerequisites for loading a companion file.

Pass --load-witness to the prepared runtime fixture to exercise this stage. The fixture uses disposable authored .ess files and checks that ordinary and save wrapper results stay intact.

## Integration conditions

The optional fresh-save notification stager preserves each reviewed engine call and forwards the original argument at the factory boundary, after the original return address has been placed on the guest stack. It brackets the save slot and final close, reports the true VM save, flush and CRT returns, and ends the attempt on all ordinary returns from the fresh-save routine. Nested calls capture the already-authorized attempt before calling the engine and report their result with that same token. These notices carry no descriptor by themselves. The app still needs to bind the factory's path to the selected profile's saves directory, and observe the exact native handle at the real file wrapper before any bytes are written or read.

Only the four reviewed output units are staged. The pinned stage test compiles those units and reverses the insertions byte-for-byte. No game-derived C is published. Run on Linux or WSL with `SKYRIM_TEST_OWNED_CLASSIC_LIFT` pointing at the completed private generation.

The combined experiment includes the reviewed selected-load notices. Save and load share one translation unit. The stager verifies the selected-load function is unchanged by save staging before inserting the load notices and the test checks the remaining bytes are identical. None of the four units overlaps the current optional MOD stages.

`save_attempt.c` accepts those notices after the caller authenticates the guest call frame and resolves its path with the actual file mapper. It checks that the mapped temporary file is a direct child of the selected profile directory and uses that attempt's same-thread lifecycle scope and file observer. At the normal return it borrows the matching read-only descriptor and the VM and slot trace for one synchronous result. Repeated VM returns, false or short writes, failed VM, close or rename results, and replaced output produce no report. The prepared-runtime test drives this adapter with authored notices and real staged file shims. The pinned lifted fixture separately checks the original save tail with the same shims. The live guest binding and immutable handoff to the companion store still need to be integrated.

`classic_save_attempt_begin_guest` checks the runtime thread's actual CPU and stack bounds, the reviewed factory return and arguments, and the path stored inside that frame. It copies a bounded, terminated input and uses the original `rt_host_path`. The hosted test uses the real staged TEB setup and mapper with a nested profile directory. A valid path opens a witness without producing a file. Foreign CPUs, incorrect factory markers, out-of-frame or unterminated names, and paths outside the authorized saves directory cannot open a witness.

```sh
python3 -B -m unittest discover -s tests -p test_stage_save_notifications.py -v
```

The observer tracks one owned fresh save at a time. A tagged operation from another thread invalidates that observation without dereferencing the owner's state. A late operation from an old token leaves the newer observation alone. The inherited handle table still requires normal guest handle ownership. The observer cannot repair a race between file lookup and close. The active save/profile must supply the authorized directory and per-attempt lifetime explicitly, including cleanup on nonlocal exits and before shutdown. Concurrent writers using independent handles require separate coverage and serialization evidence.

The staged hot paths snapshot primitive values before blocking operations. They take no new locks around the syscalls and call the observer on writes only for a tagged file. The reader and file identity checks are limited to the owned save. Before dispatch to a background worker, the caller still needs an immutable snapshot and an authenticated event handoff. The experiment leaves actual iOS scheduling, APFS capture, cached streams and physical durability for separate verification.
