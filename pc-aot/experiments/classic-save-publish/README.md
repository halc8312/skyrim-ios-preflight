# Fresh `.ess` publication experiment

The hosted fixture uses the reviewed fresh-output branch of `f_6814D0`, the complete `f_680190` close and publication body, the original `BSFile` destructor dispatch, and the CRT move wrapper. An authored file backend performs real writes, closes and renames in a new private directory. Staged game-derived C and binaries stay in the ignored `private/` directory.

## Verified Classic call chain

| Call | Observation |
| --- | --- |
| `f_6814D0` → `f_680040` with flag 1 | The fresh output branch adds `.tmp` to `<slot>.ess`. The branch with a pre-existing `manager + 0x30` stream skips the fresh setup. |
| `0x681653` → `f_677A40` → `f_6776C0` → `f_A57EA0` | The factory can open the `BSFile` during construction. Its write mode tries `r+b`, then `wb`. The adjacent `0x681667` check via `f_677540` can reuse the existing handle. |
| `0x6816A3` → `f_67E020` | The header path can render the preview via `f_69F950` → `f_69F810`, which writes three bytes per pixel through the inherited save stream. The preview is embedded in that stream and occurs before the slot call. |
| `0x6816B2` → `f_679200` | The official SKSE save-slot hook target takes the base stream here and returns void. The SDK sets its transient save name before the call and clears it on return. An owned event key has to survive that return until the base file is published. |
| `0x679534` → `f_67D220` → `f_67C340` → `f_8D4FC0` → `f_8D3750` → `f_8CD8F0` | Record `0x3E9` dispatches to the VM. Its original `0x8D3851` call returns a Boolean in AL. `f_67C340` later sets its own AL to true independently. |
| `0x6816C8` → `f_680190` → `f_677520` | The closing virtual slot is `f_A579B0`, which calls `f_B29850` to flush and `f_F56351` to close. The destructor discards both results. The CRT close route reaches `KERNEL32!CloseHandle` at `0xF67FC2`. |
| `0x6802F8` → `f_F53F06` | Rotates an old file to `.bak`. Its success cannot certify the new base save. |
| `0x680317` → `f_F53F06` | Moves `<slot>.ess.tmp` to `<slot>.ess` after closure. The CRT calls `KERNEL32!MoveFileA` at `0xF53F11`, which maps to POSIX `rename()` in the examined host backend. |
| `0x6816ED` → `f_685790` | An optional `.txt` export with `ShellExecuteA`. Even successful export can follow failed base publication. The outer routine later explicitly sets AL to true. |

The file witness acquires a read-only handle to the temp file while the writer is still alive and compares both descriptors. It requires a matching per-open generation, complete observed writes, and successful flush, CRT close and POSIX close results. The final move scope compares its source to the retained inode before the rename and its destination afterward. An identical file substituted under either name fails. The exact reader survives the writer close and rename without a late reopen of the final path.

## Run on a host

From `pc-aot`, use a completed original-body generation. The optional Linux composition uses the existing SQLite and OpenSSL dependencies. For nonstandard development headers or libraries, pass `--cflag` and `--ldflag`.

```sh
python3 -B experiments/classic-save-publish/test_save_publish.py \
  --generation /path/to/private-generation --sanitizers

python3 -B experiments/classic-save-publish/test_save_publish.py \
  --generation /path/to/private-generation --sanitizers --compose
```

The composition obtains the actual observed slot ticket while the slot call is active and stages one authored owner record. At the final rename return, it passes the held reader to the existing synchronous companion bridge. Only a verified base publication with a true nested VM result commits the record. The test reads the committed version back through the generation store and verifies that a different launch scope cannot publish it. Short writes, flush or close errors, wrong owner generation, file substitution or mutation, and rename failures all reject the companion while preserving the original engine returns.

The recorded slot call and the VM argument setup/return branch come from the pinned lift. The VM routine itself and the backend are authored. This confirms the fresh save publication sequence and synchronous capture on a host. The real backend still needs scoped provenance and authenticated per-event ownership. The examined backend's `WriteFile` can report true on a short write, `CloseHandle` discards the POSIX close result, and `FlushFileBuffers` is a success stub. The deferred path still needs an immutable snapshot and on-device validation. Physical durability and the pre-existing-stream route need separate evidence.
