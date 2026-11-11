# Private translation preparation

`scripts/prepare_translation.py` creates a **plan and input-pair manifest only**. It never executes an EXE, unpacks it, installs dependencies, copies game inputs, runs regeneration or builds an iOS application. It reuses the read-only PE/hash/version inspection from `steam_inputs.py`.

Run this first without inputs to record the current blockers in a new ignored directory:

```powershell
py -3.12 scripts/prepare_translation.py --output local/translation/preflight-001
```

The current repository profile is deliberately unreviewed, and no matching game inputs or Capstone/pefile environment have been established. Exit **2** and `status: blocked` are expected. Exit **1** is a request/error, including an existing output directory; exit **0** means only that the recorded plan's generation prerequisites passed. It never means translation or gameplay succeeded. Every output directory must be new and inside this workspace's ignored `local/`, `private/` or `generated/` paths; old failure evidence is preserved.

Once independently obtained matching private inputs exist, supply all three explicit paths and the exact intended SDK name:

```powershell
py -3.12 scripts/prepare_translation.py --original "<original TESV.exe>" --unpacked "<separate TESV_unpacked.exe>" --recipe "<private recipe.json>" --sdk iphoneos26.2 --output local/translation/pair-001
```

`iphoneos26.2` is an example, not an installed-SDK claim. Use the exact SDK selected on the build Mac. The script records it as unverified; a later Mac check must verify SDK/device support. Perform the generation preflight under the Python/POSIX host actually intended to run the upstream shell pipeline (WSL is usable for generation). The shell's `python3` path must match the preflight interpreter; activate the intended environment before running. Windows-native planning explicitly reports that it has not validated a POSIX generation host.

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
