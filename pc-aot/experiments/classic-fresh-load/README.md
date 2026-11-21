# Fresh selected-save load experiment

The hosted fixture runs the original lifted `f_6821C0` selected-load function around authored file and engine services. The staging script checks the completed private generation, exact source hashes and exact anchors. It writes staged game-derived C and binaries only under the ignored `private/` directory.

## Reviewed call sites

| Original instruction | Role in this experiment |
| --- | --- |
| `0x682220` → `f_677540` | First selected-save open. With its original return token pushed, the resolved path is at `esp + 0x14`. The fresh route eventually calls `CreateFileA` via `f_F6BCF3`. |
| `0x682275` → `f_67B720` | Original slot load. The call's low return byte controls the original success branch. A positive input read inside this call is required for a descriptor report. |
| `0x8D69A7` → `f_8D5DF0` | Load-specific VM revert. The composed fixture runs the original lifted caller `f_8D6990`. |
| `0x8D6B79` → `f_8D3DB0` | VM load. The composed fixture runs its original argument setup, call and return branch extracted from `f_8D69C0`. |

The file witness accepts the resolved `.ess` path, stream, read-only descriptor, positive read, and per-open identity from the same attempt. The successful report borrows a retained descriptor during the callback. Cached `manager + 0x30` input, metadata-only reads, changed or substituted descriptors, failed loads and reentrancy cannot yield a ready report. Original engine returns and cleanup still run.

For the normal loose-file route, the mode-zero construction before the first selected open performs a metadata query. The selected-open call at `0x682220` reaches the real `CreateFileA` through `f_677540` and the original `rb` CRT path. The return token is `0xF6BF24`, with `GENERIC_READ`, `FILE_SHARE_READ`, and `OPEN_EXISTING`. The stock slot call has a logical stream read at `0x67B870` returning at `0x67B875`. The CRT can fill its buffer before the slot begins, so a positive `ReadFile` during the slot is insufficient as the sole acceptance condition. The app binding must connect the scoped native file handle's generation to the same stream and CRT file across a positive logical read, before the existing immutable handoff. The hosted fixtures do not establish that live identity chain.

## Native selected path resolver

The fresh branch of the pinned PE uses the same 260-byte stack path for the name builder at 0x6821FC and the stream constructor at 0x68220C. After the first open pushes return 0x682225, the path is at ESP + 0x14 and the selected stream is in both ECX and ESI. The original caller allocates 0x20C bytes for its local frame. The later check call at 0x682246 has a different return address.

The read-only resolver checks the current runtime CPU and its TEB stack bounds before reading that frame. It uses the real Skydream path mapper to preserve the spelling on disk and accepts a regular .ess directly in the caller-authorized profile directory. It clears the result on refusal and does not open the file. The selected-load witness must still authenticate the actual normal open and stream identity.

The prepared classic-runtime-save fixture runs the resolver with the real mapper and thread setup against authored files in a temporary profile. It checks missing paths, foreign or malformed frames, traversal, a sibling directory, a nested file, a directory and a symlink. The original selected-load lift fixture remains independent. The normal app has no resolver binding.

## Preloaded stream

The pinned Classic PC executable identifies the reviewed producer as `CopySaveGamesFromHost`. Its two direct calls are in the `ForceSave` command handler `f_51D390`. It enumerates `Saves/` and opens each candidate `.ess` via `0x6808F3 → f_A58870` and the `CreateFileA` call at `0xA58514`. The path comes from that enumeration. The consumer at `0x6821EC` adopts the current `manager + 0x30` stream before it can run the selected-name open. The dedicated `SysUtilLoadCompleteCallback` wrapper `f_682390` passes the stream itself as the name at `0x6823AA`, then destroys the stream and clears the field at `0x6823E4`. The callback installed by the producer, `f_67FEA0`, can also destroy the stream and clears the field at `0x67FF00`.

The original PE and lift agree on the following blockers to a positive hosted witness.

- The producer queries size with `GetFileSizeEx`. It calls `f_A580A0` at `0x680981`, which passes the low 32 bits of the requested length to `ReadFile` at `0xA580DC`. The producer tests the Win32-derived status before storing the stream at `0x680BA5`. It does not compare the reported completion count to the full file size. The current runtime's `ReadFile` wrapper can return success with zero or short output.
- At `0x68094E` the producer calls the stock `f_6776C0` stream constructor. In file mode it allocates a `0x15c`-byte backing object through `f_A57EA0`. The other branch leaves the backing pointer null. The producer expects a backing buffer at `+0x160` and writes transfer metadata farther into that object. The separate memory-file constructor `f_A59C70` owns a buffer at that offset, and there is no verified call to it on this Classic PC route.
- After the store at `0x680BA5`, the PC platform object's virtual slot at `+0x20` resolves to the immediate-return body `f_6C50E0`. The direct producer closes its temporary file through `f_A58800`. These observations leave no verified PC transfer-completion event or retained descriptor to hand to the companion bridge.

If a working producer is observed on the target, the smallest companion witness needs one single-use lease per copied file. At the exact scoped open, bind the authorized selection and profile to a generation and retain the same immutable read-only descriptor before the first read. Track the native handle generation, complete 64-bit size within the reader's range, actual bytes read from offset zero, and the validated backing allocation and interval. Publish that lease only with the original manager store after a complete read into that interval. Carry the lease across any worker handoff. At cached adoption, require the same selection generation, manager, stream and backing lifetime plus an actual bounded read from that interval in the slot call. Invalidate on replacement, original destruction, cancellation, new selection or attempted consumption. Match the original slot and VM results to the same lifecycle ticket before returning any companion bytes. An unrelated fresh load follows its existing route.

The persisted roundtrip fixture checks the negative case with an already-open authored stream. The original slot and VM can succeed while the bridge performs no new capture, hashing or companion restore. A positive fixture that constructs the missing backing object or invents a platform callback would only prove its authored services.

## Run on a host

Run the following from `pc-aot` with a completed original-body generation. The basic mode needs a C compiler and, on Linux, OpenSSL development files. The composition additionally needs Linux `memfd` seals and SQLite development files. Nonstandard library locations can use `--cflag` and `--ldflag`.

```sh
python3 -B experiments/classic-fresh-load/test_fresh_load.py \
  --generation /path/to/private-generation --sanitizers

python3 -B experiments/classic-fresh-load/test_fresh_load.py \
  --generation /path/to/private-generation --sanitizers --compose
```

The composition uses the existing companion bridge. The authored selected input has kernel write, grow and shrink seals. The file backend uses the returned read-only descriptor from offset zero and verifies that the original descriptor's cursor stays independent. It reads an authored byte during preflight and a second byte inside the original slot scope. The existing lifecycle recorder produces the slot and nested VM rows at the reviewed lifted calls. A result matching all of that evidence returns the companion bytes previously committed for the authored input. A false VM return, missing VM call or revert, shifted slot ticket, different consumed descriptor, false slot return or missing positive slot read returns no companion.

The composition snapshots the next recorder ticket in its single-thread fixture and verifies the actual reservation before acceptance. Runtime integration still needs to bind the same freshly opened input to a load attempt before the loader's first read, coordinate its ticket across threads and capture the matched VM calls on device. Cached/memory-backed inputs and storage immutability on iPhone remain unverified. All nested engine callees and base save bytes in this test are authored, and the experiment has no app entry point.
