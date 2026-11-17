# Private shader input study

`study.py` creates a bounded inventory of candidate shader records from a privately
held Skyrim 2011 BSA. It does not modify the archive, execute game code, call DXSO's
native parser, translate shaders, run GPU work, or change application settings.

The first supported archive subset is named BSA v104 with sequential folder
blocks. All index, count, range, overlap and decoding limits are checked before
using data. Compressed output is capped during decompression. Unsupported flags,
versions, layouts, sizes and malformed streams are explicit failures.

Candidate discovery uses `BSVS`/`BSPS` markers with independently checked record
size, stage, SM3 token bounds, relative operands, declaration/definition shapes,
and END position. Overlapping candidates are rejected. **This is not a complete
FXP grammar or D3D shader validator.** Acceptance permits private key comparison;
it does not establish native-compiler safety, runtime use, or coverage.
An archive-wide 64 MiB validation-work budget also limits repeated parsing of
overlapping marker candidates before they can be rejected for overlap.

For accepted candidates, key construction follows the fixed Skydream/DXVK source
at `ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a`: D3D9 stage plus SHA-1 of the
bytecode from the version token through the END token, including comments.
`dxso_decoder.cpp` determines read order, `DxsoModule::runAnalyzer` includes the
version token, and `D3D9ShaderModuleSet::GetShaderModule` hashes exactly that byte
count. The core decoder/key source files have LF-normalized hash guards; raw
hashes are also retained in reports. Names, hashes, offsets, manifests and any
extracted bytecode are private.

Run the synthetic tests before using an owned copy:

```powershell
python -m unittest discover -s tests -p test_shader_input_study.py -v
python experiments/shader-inputs/study.py --archive '<private owned Shaders.bsa copy>' --output private/analysis/shader-input-study-01
```

`native_check.py` separately builds a small Linux/WSL oracle from the pinned
DXSO decoder/analyzer. Its only inputs are authored literals compiled into the
binary; it has no bytecode/file argument. It first checks those literals using
the bounded reader, then compares the actual DXSO byte count, stage and SHA-1.
Use `--sanitizers` with the existing compiler to enable ASan/UBSan:

```sh
python3 experiments/shader-inputs/native_check.py --sanitizers --output private/analysis/shader-input-native-01
```

This establishes agreement for the synthetic cases, not semantic validity or
native safety for every accepted game candidate. Real game data is never passed
to that oracle. Compilation and execution logs remain in the fresh private run.

The output path must be new and below this repository's ignored
`private/analysis/`. `--extract` optionally writes deduplicated `.dxso` candidates
there, using stage/hash filenames, never embedded archive paths. An existing
output is refused. The input is read-only and its identity and SHA-256 are checked
again after the study. Failure evidence remains in the new private directory.

The report deliberately distinguishes archive entries, marker candidates,
accepted stage/hash keys and rejected/unknown records. It cannot claim all game
shaders, all render states, fixed-function shader coverage, production option
parity, or actual `.dxvk-cache` matches. Those require separate evidence.

`cache_index.py` reads a privately copied DXVK v10 state cache. It checks the
fixed header, record checksums, stage keys and packed field lengths before
writing a key index. It refuses other versions and malformed or oversized input;
it does not repair a cache or load it into the game. Its native comparison test
uses the pinned writer with authored state, including all six shader stages.

```powershell
python -m unittest discover -s tests -p test_shader_cache_index.py -v
python experiments/shader-inputs/cache_index.py --cache '<private device .dxvk-cache copy>' --output private/analysis/shader-cache-index-01
```

`join.py` compares a completed inventory with a private `cache_index.py` result
for the same pinned revision and v10 cache format. It validates bounded JSON,
duplicate fields, exact scope flags, source declarations, stage/hash consistency,
record dimensions, offsets and recomputed counts before joining. It never reads
bytecode or replays recipes. Raw cache checksums and bytecode are **not revalidated
by the join**, and declared source hashes are not authenticated provenance.

```powershell
python -m unittest discover -s tests -p test_shader_input_join.py -v
python experiments/shader-inputs/join.py --inventory '<private inventory.json>' --index '<private index.json>' --output private/analysis/shader-key-join-01
```

The report separates saved-record counts from unique recipes and reports how
many have every stage/key present among the candidates. An empty cache has an
undefined fraction, not 100% coverage. Unknown keys remain unclassified; they
are not assumed to be fixed-function shaders. Duplicate candidates and saved
recipes are not gameplay frequency or a new performance saving. No device cache
has been joined by the synthetic tests, so they establish no actual coverage.
Input manifests and all joined hashes stay private. Output paths must be fresh
and resolve below the literal private analysis directory, including when a
parent directory is a symbolic link or junction.

Archive layout was cross-checked against the primary OpenMW reader:
https://github.com/OpenMW/openmw/blob/master/components/bsa/compressedbsafile.cpp
