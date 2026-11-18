# Private translation preparation

`scripts/prepare_translation.py` creates a **plan and input-pair manifest only**. It never executes an EXE, unpacks it, installs dependencies, copies game inputs, runs regeneration or builds an iOS application. It reuses the read-only PE/hash/version inspection from `steam_inputs.py`.

Run this first without inputs to record the current blockers in a new ignored directory:

```powershell
py -3.12 scripts/prepare_translation.py --output local/translation/preflight-001
```

The current repository profile remains deliberately unreviewed. The first completed Japanese Steam input is1.9.31.0, outside the documented1.9.32 target; its original and complete Data copy are preserved privately. An approved Capstone/pefile environment now exists as described below. Exit **2** and `status: blocked` are still expected until all input/profile gates pass. Exit **1** is a request/error, including an existing output directory; exit **0** means only that the recorded plan's generation prerequisites passed. It never means translation or gameplay succeeded. Every output directory must be new and inside this workspace's ignored `local/`, `private/` or `generated/` paths; old failure evidence is preserved.

Once independently obtained matching private inputs exist, supply all three explicit paths and the exact intended SDK name:

```powershell
py -3.12 scripts/prepare_translation.py --original "<original TESV.exe>" --unpacked "<separate TESV_unpacked.exe>" --recipe "<private recipe.json>" --sdk iphoneos26.5 --output local/translation/pair-001
```

`iphoneos26.5` is the SDK used by the verified probe cloud builds. A later game build must verify its own Mac SDK/device support; this Windows/WSL planner records the intended SDK as unverified. Perform the generation preflight under the Python/POSIX host actually intended to run the upstream shell pipeline. The shell's `python3` path must match the preflight interpreter; activate the intended environment and invoke `python3` before running. Windows-native planning explicitly reports that it has not validated a POSIX generation host.

## Approved private dependency environment

`private/tooling/skydream-py312-20261112/` is a WSL Python3.12.3 venv created with `--without-pip`. The already-installed Windows Python's bundled `pip-25.0.1-py3-none-any.whl` was reused through `PYTHONPATH` for installation only; no new pip/system package was downloaded. The user approved only Capstone5.0.9 and pefile2024.8.26. Their official release-specific PyPI metadata supplied filenames/sizes/SHA-256s, checked before installation. Original wheels, `downloads.json`, `install.log` and verification records remain in `private/tooling/wheels-20261112-skydream/`.

The locked install used `--no-index --no-deps --only-binary=:all: --require-hashes`, the preserved wheel directory and `requirements.translation-linux-x86_64.txt`. No global Python environment, iOS app dependency or game input was changed. From WSL, activate the prepared environment before preflight/regeneration:

```sh
cd /mnt/f/SkyrimIOS
. private/tooling/skydream-py312-20261112/bin/activate
python3 scripts/prepare_translation.py --sdk iphoneos26.5 --output local/translation/<new-run>
```

The no-input example still returns blocked, by design. Pip metadata correctly identifies Capstone5.0.9; its upstream Python `__version__` constant remains5.0.7. The byte identity and pinned official-source explanation are recorded in SOURCE_AUDIT.md. Preserve this discrepancy; do not rewrite the package to make its display match.

The recipe is a UTF-8 JSON record of the actual independently performed preparation, with `method`, `tool`, `tool_version`, a nonempty `steps` array of strings, `original_sha256` and `unpacked_sha256`. Both hashes must match the supplied files. For a genuinely already-unpacked original, record that fact and a separate byte-identical copy as the derived input. Recipe text is recorded and hashed, never executed; it is evidence, not automatic certification.

`input-profile.json` is the fixed, version-controlled compatibility gate. Its `reviewed_unpacked` is currently null. Correct filename, version, 32-bit PE layout, image base or an upstream SHA-1 prefix cannot approve a binary. A future reviewed profile must bind this exact translator revision to the unpacked SHA-1, SHA-256, entry RVA, section-metadata digest and review evidence for the fixed-address hooks. There is no caller override, profile selector or `--allow-unknown` option. Unsupported architecture, a remaining `.bind` section, modified/wrong translator checkout or mismatching reviewed identity stays blocked.

The deterministic contract digest covers translator revision, profile digest, separate input SHA-256s, recipe digest, target and fidelity settings. The manifest additionally preserves separate input paths, SHA-1s, PE metadata, version-resource results, current interpreter/dependency state and blocker details. It contains private information and must not be committed.

After the reviewed-profile and dependency gates pass, a future staging step must use `git archive` at the pinned revision to create a fresh canonical-source tree under the run's `work/skydream/`, copy the approved unpacked input to `inputs/TESV_unpacked.exe`, and reverify identities. Do not filesystem-copy a Windows CRLF checkout into the POSIX shell pipeline. The exact **planned**, unexecuted command from `work/skydream/` is:

```sh
env LIFT_MEMORDER=1 LIFT_TSO=1 DISCOVER_JOBS=2 sh tools/regen_gen.sh ../../inputs
```

Outputs belong in that private copy's `analysis/` and `gen/`, including `gen/image/`. Keep generation logs and reject counts. Compilation must carry `LB_STRICT_ORDER=ON`, `LB_GEN_JOBS=2` and total heavy build concurrency 2. These settings improve the fidelity baseline but do not establish CPU equivalence. The iPhoneOS dependency closure, app integration, signing and physical-device tests remain separate work; no desktop CMake preset is advertised as an iOS build.

Negative and provenance tests use synthetic PE fixtures only:

```powershell
py -3.12 -m unittest discover -s tests -p test_translation.py -v
```

Initial local result on 2026-11-12: all 10 tests passed. Real no-input plans are preserved under `local/translation/audit-no-inputs-20261112/` and `audit-wsl-no-inputs-20261112[-v2]/`; WSL explicitly returned exit 2. The first WSL source check timed out; the retained retry identified the pinned SHA but a dirty checkout under WSL Git because the Windows checkout uses CRLF. No vendor files were rewritten to hide that mismatch. Missing private inputs, unreviewed profile and missing pinned Python packages independently block generation.

The first real macOS CI exposed `/var` versus `/private/var` canonical-path differences. Root normalization was fixed and two symlink regressions added, including rejection of output escaping private storage. The current translation suite has 12 tests: all 12 passed on WSL and macOS; Windows passed 10 and skipped the two tests needing unavailable symlink privileges. Raw original CI failure is retained as listed in STATUS.md. These tests use synthetic inputs and do not establish translation correctness for TESV.exe.

After input installation, the shared Windows/WSL checkout preflight was corrected to inspect Git status with per-call `core.autocrlf=true`, matching the Windows checkout's normalization. The exact Git executable/version and override are recorded in each plan; vendor files, index and configuration are not rewritten. Six real-Git fixture tests show that source/staged/untracked/binary changes and wrong pins still block. The expanded translation checks passed18 on WSL and16 on Windows, with the two existing Windows symlink-privilege skips. Actual WSL inspection of the pinned checkout passed, with unchanged index/config hashes. Canonical `git archive` staging is still required before regeneration; this change does not permit executing arbitrary worktree changes.

## Ordered translator patches

The AF correction in [CPU_VALIDATION](CPU_VALIDATION.md) is pinned under the Skydream source's ordered `patches` array in `source-lock.json`. Preflight checks every path, base revision and actual SHA-256; the contract includes the complete ordered identities so changed/reordered patches invalidate generation reuse. It never executes patch text. The planned archive command explicitly sets `core.autocrlf=false`. Inside a new archive only, the planned patch commands initialize isolated Git management, then check/apply each patch in order with the same normalization. An outer repository must not capture patch paths. Stop on any mismatch or failed command.

Eight synthetic patch-provenance tests cover changed bytes/order, invalid metadata/paths, symlink escapes and actual isolated application of a dependent patch pair without changing vendor or outer files. The combined translation suites passed26/26 on WSL; Windows passed22/26 with four symlink-privilege skips. Successful identity checks do not imply that the game-input profile is approved or that arbitrary patches are safe.

## Diagnostic discovery before profile approval

`scripts/run_discovery.py` runs only the pinned `discover.py` and `peimg.py`, extracted from Git. It has no lifter, address-override, image-generation, rebasing or regeneration step. It snapshots the candidate and verifies the explicitly supplied SHA-256, records the Python/dependency/source hashes, and leaves `profile_approved=false`. The translator AF patch is deliberately not applied to this independent discovery stage.

```sh
cd /mnt/f/SkyrimIOS
private/tooling/skydream-py312-20261112/bin/python3 -E -s -B scripts/run_discovery.py \
  --input private/translation/unpack-steamless-20261112-defaults/TESV.exe.unpacked.exe \
  --sha256 d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605 \
  --output private/discovery/<new-run> --timeout-seconds 21600
```

Two workers are fixed. Six hours is a deadline, not an ETA; total RAM is not bounded. The upstream program only writes final JSON at the end and has no checkpoint/resume. The runner distinguishes missing/truncated/schema-invalid output from completed discovery, preserves every attempt, and forwards cancellation to its owned process group. Nine runner tests passed on WSL; Windows passed5 with4 platform skips. A later archive-normalization regression passed on both hosts without repeating real discovery.

Real candidate analysis completed at `private/discovery/basepin-english-1932-20261112-01/`, exit0, 16:58:52–17:05:08 JST. It recorded90,900 function candidates,1,083 jump tables,1,201 unresolved indirect jumps and4,029 gaps totaling46,802 bytes. Both JSON files parsed, and original/snapshot hashes remained unchanged. These counts do not establish complete coverage or compatibility approval. The actual source bytes and dependency inventory in `result.json` remain fixed even if wrapper documentation subsequently changes.

The original1,201 unresolved-list entries represent1,051 unique instruction sites. A later bounded audit found two empty inferred jump tables omitted from that list (`F5386C`, `F52EAC`). `0002-report-empty-jump-tables.patch` makes these failures explicit without inventing targets or changing graph/table/range behavior. Three synthetic regressions passed in the approved WSL environment; Windows passed patch verification and skipped two decoder/fork tests. The supplemental read-only union has1,053 unique unresolved sites; it is recorded in `private/analysis/english-contracts-20261112/empty-table-supplement.json`. Original discovery output was not rewritten or rerun, and the table destinations remain unknown.

## Four-function contract diagnostic

Independent block inspection found all20 selected roots (16 host overrides plus4 motion functions) and complete decoding of their recorded blocks. Only four string roots qualified as self-contained for the following diagnostic: `F5B670`, `F520F0`, `F55A90`, `F59140`. Their exact blocks have no calls, global-data references or escaping/unresolved transfers. `F520F0` includes a shared tail below its entry, so the runner retains the discovered block sets rather than slicing entry-to-end.

`scripts/check_private_strings.py` requires an explicitly hashed independent audit, exact candidate/discovery hashes, the fixed source and AF patch, and `--execute`. It lifts only those four original bodies into a new ignored private directory and compares them with the pinned upstream native wrappers using the upstream deterministic oracle. Every unknown call/instruction aborts with diagnostic context. This is a limited diagnostic used to review contracts, not the full regeneration pipeline or an approved game build.

```sh
cd /mnt/f/SkyrimIOS
private/tooling/skydream-py312-20261112/bin/python3 -E -s -B scripts/check_private_strings.py \
  --candidate private/discovery/basepin-english-1932-20261112-01/inputs/candidate.exe \
  --discovery private/discovery/basepin-english-1932-20261112-01/analysis/functions.json \
  --audit private/analysis/english-contracts-20261112/inventory.json \
  --audit-sha256 bed0492b77d20bc4db5aec45b611d0eb7a8e05a77d70fbb39cdfe3d6fe3254f0 \
  --execute
```

The first completed run `private/diagnostics/four-strings-20261112T081458.223724Z-12/report.json` passed32,000 comparisons of return values, writes and preserved ABI. The generated C SHA-256 is `289434b8b227a57702735d751cc3a26eac61ff1d588b58bfac7030c8723b063c`. Source snapshots, all temporary C/binary files and stdout/stderr were retained; input/discovery/snapshot hashes remained unchanged. Compile concurrency is1, and the compiler/oracle have120s/60s deadlines and1GiB/128MiB address-space limits.

Four translated game-derived functions were executed in this isolated host diagnostic; the game executable was not launched. The reference is lifted C, so shared lifter errors can remain. These finite tests do not approve the other hooks, globals, full input profile, game operation, ARM64 or iPhone behavior. Memory-move helpers/unresolved tables, CRC global tables, seven other overrides and motion state require further review.

After that successful run, wrapper-only changes made canonical archive flags, resolved private paths, explicit derived-function execution metadata and scoped cancellation handling stricter. Seven game-free lifecycle/path tests passed on WSL; Windows passed4 with3 platform/privilege skips. The successful runner snapshot SHA-256 remains `695355e885ead724302d5ca9205ec526fcfe3fea972fe3f0547a6da8281ab4f1`; the current wrapper is `699e32eac46b4d28b96aba9f081adb78c5ed148aa804c2f1ea33ee5856dbc32e`. Original comparison artifacts were not rewritten or rerun for those wrapper changes.

The two empty memory-move tables have a concrete inference cause: a taken `sub ecx,4; jb` branch uses wrapped indexes−4 through−1, and its displacement points immediately after the four real entries. The existing discoverer loses taken-branch history and scans from index0 into code. The directly referenced entries are mapped/file-backed read-only text and have known block destinations. `private/analysis/english-contracts-20261112/memmove-negative-index-explanation.json` preserves the original read-only diagnosis.

## Conservative negative-index candidates

`0003-negative-index-candidate-roots.patch` adds a narrow generic rule after function exploration. An adjacent full-width `SUB reg,k; JB` taken edge directly to `JMP [reg*4+disp]` can provide candidate destinations for `k=1..32`. The rule checks all observed local predecessors, rejects bypasses, conflicting paths, calls/known entries, flag or register clobbers, segment overrides, ambiguous sections and non-file-backed bytes. Every target must decode inside file-backed executable storage and not inside another known instruction. Candidate addresses enter the existing discovery-root queue; they are not asserted to be machine CALL edges.

The dispatch **remains unresolved**, because unobserved/global entry paths cannot be proved absent by this local rule. Negative slots are not inserted into the existing zero-based `jump_tables` arrays or `jt_ranges`: doing so would mislabel table positions and gap coverage. This patch preserves that metadata and does not certify a complete switch or whole-game coverage.

The ordered patch SHA-256 is `21f610a0af715992a6b16b2adcacd0924e4d020cc7c7cd6c88beabc9338668e3`. Twelve synthetic checks passed in WSL, including serial/parallel agreement, unchanged table/range/gap metadata and closed-mapping lifetime. An earlier Windows run passed source application and skipped ten checks needing its unavailable decoder. Independent review passed20 adversarial control-flow cases and8 backing/bounds cases. Evidence: `local/tests/negative-roots-independent-20261112/report.json` and `local/tests/negative-roots-independent-bounds-v2-20261112/report.json`; earlier snapshots remain recorded rather than being relabelled as the final source.

The first real two-function probe exposed a `pefile` mmap that was already closed, making `pe.__data__` unsuitable for raw-byte lifetime checks. The failure remains in `private/analysis/negative-table-real-probe-20261112/`. The final patch retains immutable input bytes before PE mapping and conservatively skips rebased images. The corrected probe at `private/analysis/negative-table-real-probe-v2-20261112/summary.json` adds four candidate roots per memory-move function; all eight are already known block starts and neighboring-table destinations. Blocks, tables and ranges are unchanged across the pinned source, patches1+2 and patches1+2+3. Both sites stay unresolved; this was static inspection, not guest execution or a full discovery rerun.

The26 translation/preflight tests also passed (`local/tests/negative-table-integration-20261112-180031/translation-tests.log`). The first three-patch input plan at `local/translation/english-pair-three-patches-20261112/translation-plan.json` verified the earlier patch identity and is preserved. No full regeneration or profile approval follows from these checks.

The final lifetime-safe patch also passed an independent closed-mmap/rebase regression. The current real input plan is `local/translation/english-pair-three-patches-v2-20261112/translation-plan.json`: all three ordered patch hashes match, exit2, only `unpacked_hash_and_layout_not_reviewed`, no regeneration.

## Memory-move and CRC diagnostic

`scripts/check_private_primitives.py` has separate `--prepare` and reviewed `--execute` stages. It reuses the bounded process/private-path helpers from the string diagnostic, requires the exact candidate/discovery hashes, archives the pinned source plus ordered patches, and preserves original tables and all source/input identities. Nine game-free wrapper checks pass on Windows/WSL. It runs the upstream five-entry comparison, including real original helper bodies, with unknown calls/instructions remaining fatal.

The first seven-root execution at `private/diagnostics/five-primitives-reviewed-20261112-01/` stopped with `UNRESOLVED guest target=0x00f53ae4` (SIGABRT); compilation alone was not counted as success. The preparation audits01–03 and execution failure remain unchanged. Earlier audit01 also recorded compacted jump-table slots: discovery omits an invalid slot0 even though the original bytes remain present. Later preparation retains every raw slot and separately verifies the alignment guard rather than treating the compacted target list as contiguous memory.

The failure exposed another pattern: four observed `CMP ECX,8; JB` paths per memory-move function feed `NEG ECX`, producing indexes−7..0. Nonnegative discovery records only slot0 and misses seven destinations per function. All14 omitted destinations already exist as separate roots in the original full discovery. The new diagnostic selects their exact original bodies rather than copying a whole address range or using a stub. Independent inspection finds no arithmetic flag reads anywhere in those14 closures; they contain only MOV/LEA/ADD/POP/LEAVE/JMP/RET. This makes function-local flag initialization acceptable for this finite helper scope, without fixing the general cross-call flags problem.

`private/analysis/backward-memmove-cfg-20261112/` retains the cause, authoritative `flags-correction.json`, flag-use result and count supplement. The original1201 unresolved entries mean1051 unique sites; adding the2 empty and2 newly identified incomplete nonempty tables gives **1055 known unresolved unique sites**. This is a read-only supplement, not a complete new discovery result. Patch3 covers the adjacent SUB/JB candidate rule only, not this CMP/JB/NEG pattern.

Preparation04 selects21 exact roots: five primitive entries, two SSE helpers and14 backward-copy landing pads. Its audit is `private/analysis/primitive-contracts-20261112-04/audit.json`, SHA-256 `7d9be136d7f3a89f1d42b3d693f66631a5154234680807e8cf618c7e873feafd`. The original256-entry CRC table is compared byte-for-byte with the native table; reference execution still reads the original image. The diagnostic only zero-initializes an otherwise unused fourth argument in the upstream harness and improves fatal diagnostics/artifact retention; it does not change the compared algorithms.

```sh
private/tooling/skydream-py312-20261112/bin/python3 -E -s -B scripts/check_private_primitives.py \
  --execute --prepared private/analysis/primitive-contracts-20261112-04 \
  --audit-sha256 7d9be136d7f3a89f1d42b3d693f66631a5154234680807e8cf618c7e873feafd \
  --output private/diagnostics/<new-primitive-run>
```

The finite oracle uses4000 iterations across five entries, scalar/SSE paths, overlapping/aligned memory moves up to4096bytes and CRC inputs. It checks a20KiB payload window, ESP/callee-saved GPRs/DF and EAX for four entries (not `A4A530`). Initial DF/SIMD state is zero. It does not prove all flags, SIMD state, other globals, memory ordering, ARM64 or game behavior; the reference itself is lifted C and may share lifter errors. Compiler/oracle limits are120s/60s and1GiB/128MiB, one compiler at a time. The private32MiB image is not a hardware access sandbox; bad guest addresses may crash this bounded process.

Run `private/diagnostics/five-primitives-reviewed-20261112-02/` completed with exit0 and **20,000 passing comparisons**. All1,639 final input/source identity checks matched. Generated C SHA-256: `50f5f3c2e900cab5704a846085d5466be14b662c3f82fef2b5f8075eead7aa7e`; retained runner SHA-256: `53ac73449d05b2bdce994ad138f287f100be513a375237a36150f65f3500c99e`. The full raw output is `oracle-temporary/oracle-2-run.stdout`. Twenty-one game-derived function bodies were generated and available to this finite host oracle; no full game, iPhone run or input-profile approval occurred.

## Remaining fixed contracts

The static audit in `private/analysis/remaining-overrides-20261112-01/` separates the other seven overrides. The Scaleform, inventory/form UI and Papyrus wrappers retain original bodies with virtual calls, object layouts and registry/heap dependencies. Two solver observers also retain callbacks and world state. Merely forwarding to a guest body does not validate those dependencies or the incoming ABI.

The scheduler at `D0DB50` is a smaller next comparison:11 blocks,50 instructions, no calls or escaping edges and10 globals. Its cdecl arguments include a float and the low byte of the following slot; it uses stack scratch and overwrites the full second argument slot. One remainder global is mapped zero-fill rather than file-backed and must be explicitly initialized. A finite comparison must set x87/control state, stack canaries and a deterministic single-thread clock. The upstream adaptive test extracts the actual translated body, but compares that same body through a wrapper; it is not an independent x86 oracle. Its unexpected calls abort, and an outer timeout and retained artifacts are still required before executing it.

The separate four-motion audit in `private/analysis/remaining-physics-20261112-01/` covers195 blocks and1,616 instructions. Five original clock reads match the preparer's expected counts. The bodies still contain109 calls, including29 unresolved dynamic calls and four TLS imports. Their mutable object/vtable state prevents treating a synthetic fixture as full-body equivalence. Existing preparation and fixed-bridge tests use synthetic inputs or guest callbacks. A22-byte call-free clock/FP-branch slice is a possible later diagnostic, not proof of character motion or fixed-step gameplay. These call counts are separate from the known unresolved-jump count and are not added to it. Neither audit generated or executed game-derived code or approved the input profile.

## Preserve the native audio source

`0004-preserve-native-audio-source.patch` corrects an independent build defect: the `SKYDREAM_SKSE=OFF` branch reset the runtime's `SOURCES` property to its filtered runtime list, omitting the explicitly added `F3DAudio.c`. The patch retains that source while keeping the existing filters and enabled-SKSE path unchanged. It adds no dependency and does not enable SKSE, SkyUI or experimental physics.

`tests/test_desktop_audio_sources.py` evaluates verbatim upstream CMake command spans with synthetic empty source files and no enabled compiler. Two tests cover five configurations: baseline OFF/ON and patched OFF/ON/ON+SkyUI. They verify the source list, filters and definitions, not compilation, audio behavior or iPhoneOS linking. Baseline failure evidence is retained at `local/tests/desktop-audio-source-20261112/baseline-evidence/`. The patch hash is `24e8e69260880dc5c5fd9b6d6ca35e405c9cde5ad3898ea59695a9b15254cee4`. Source preflight allows this exact CMake path; traversal, other build files, binary and game-derived payload checks remain active, with13 related Windows checks passing. Earlier CPU diagnostic snapshots keep their original one- or three-patch identities; this build-only fourth patch does not relabel those results.

The current full-generation plan is `local/translation/english-pair-four-patches-20261112/translation-plan.json`. All four patch hashes match; it remains blocked solely by `unpacked_hash_and_layout_not_reviewed`, with `executed:false`. It does not run the planned regeneration or Apple build.
