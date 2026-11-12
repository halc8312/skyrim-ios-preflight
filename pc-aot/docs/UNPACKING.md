# Private Steam input preparation

The English Steam original was acquired through the user's own client after an approved per-game language change. Both complete Japanese and English installations were independently copied and inventoried before further processing. No game executable was launched. Original inputs, derived outputs and all attempts remain under ignored private paths.

## Source-built static unpacker

Steamless source is pinned to `cd770bf9749d3e4f438d23ac643917ad1a804257` from [atom0s/Steamless](https://github.com/atom0s/Steamless/tree/cd770bf9749d3e4f438d23ac643917ad1a804257). The source was materialized with `git archive` into `private/tooling/steamless-cd770bf-20261112/source/`; the vendor checkout remains unchanged. Steamless carries [CC BY-NC-ND 4.0](https://github.com/atom0s/Steamless/blob/cd770bf9749d3e4f438d23ac643917ad1a804257/LICENSE). Tool binaries and this retargeted local build stay private.

The installed MSBuild17.13.19.12613 / C# compiler4.13.0-3.25167.3 built the CLI and x86 variant plugins without NuGet restore or new SDK downloads. The initial .NET4.5.2 build failed because reference assemblies were absent; its original `build.log` is retained. A command-line `/p:TargetFrameworkVersion=v4.8` override used existing4.8 reference assemblies without editing source/project/config files. Exact commands, compiler, source materialization and output hashes are in `private/tooling/steamless-cd770bf-20261112/steamless-audit.json`.

The actual input matches **SteamStub2.1 x86**, not3.0. CLI3.1.0.0 uses API1.0.0.8 and Variant21.x86 plugin1.0.0.9. Its tracked SharpDisasm1.0.0.0 dependency has SHA-256 `6d3ce990cdf58da228697d25416d16d15994135c5f66571fe1e00e9c975bc2cf`, pinned Git blob `bbddf493ab56c4addd5a3eba6b7786f3d97416e4`. Its [BSD-2-Clause license](https://github.com/justinstenning/SharpDisasm/blob/master/LICENSE.md) and repository-binary origin were inspected; correspondence to a reproducible SharpDisasm source build was not established. No separate dependency download was needed for the tool.

Source review and the compiled `CanProcessFile` check identified the supported variant without calling `ProcessFile`. The reviewed processing path reads, decrypts and disassembles input bytes statically; it does not execute TESV code. Before running, every built tool/config/plugin/library hash was rechecked against the audit.

## Original and default candidate

| Role | Size | SHA-1 |
|---|---:|---|
| English Steam original, PE file/product version1.9.32.0 | 18,024,240 | `f39aecebf6e332616c4c518617073f6e8250e0f8` |
| Steamless default candidate, same version | 17,462,064 | `17ec1ef6d72781359860f103f600711dfc219182` |

Original SHA-256: `69362f77bc56bf17570a33106d020b6c67fd8f32a294b82f8b687b9cf71efc72`. Default candidate SHA-256: `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`.

The original's SHA-1 prefix matches the packed input documented upstream. Its resource version has four fields; it does not literally display the five-field `1.9.32.0.8` prose label. The complete input identity above is retained instead of changing a version label to make it agree.

Each attempt creates a new directory and a private copy named `TESV.exe`. Verify its hash and ensure `TESV.exe.unpacked.exe` is absent, because upstream uses overwrite mode for its output. The default invocation was:

```powershell
& 'F:\SkyrimIOS\private\tooling\steamless-cd770bf-20261112\source\Steamless\bin\x86\Release\Steamless.CLI.exe' '<new-private-attempt>\TESV.exe'
```

Defaults remove `.bind`, zero the DOS stub and checksum, preserve raw alignment, and disable optional payload/DRMP dumps and experimental mode. Original/snapshot/attempt input hashes were checked again afterward and remained unchanged. `private/translation/unpack-steamless-20261112-defaults/recipe.json` binds exact source/tool/arguments/options to separate original and output hashes; `unpack.log` is preserved. Exit code0 and output presence were followed by PE/version/hash inspection, not interpreted as game success.

The default output has six sections and entry VA `0x00f568a1`. Pinned Skydream `embed_game_image.parse_image` accepts its PE/import/TLS structure, finding266 imports; this is a structural check, not hook/function compatibility. The 16 audited hook-position fingerprints differ from the Japanese1.9.31 input, confirming that mapped locations alone cannot justify reusing those hooks on the Japanese version.

## Remaining identity discrepancy

At the pinned Skydream source, `README.md:186` says a local build exercised TESV SHA-1 `67a597df3bb8c5db94b9032f8179d29631b1145d`, without an explicit original/derived role or recipe. Retained `docs/UPSTREAM_README.md:47–50` specifically names packed prefix `f39aeceb` and Steamless-unpacked prefix `fed96638`. The default output above matches neither unpacked claim.

Eight supported formatting combinations of defaults/`--keepstub`/`--realign`/`--recalcchecksum` succeeded in separate private attempts. None produced either upstream hash. All output bytes from file offset0x400 through EOF are identical, including a6,448-byte overlay. Keepstub changes138 DOS-stub bytes; checksum changes four header bytes. Realignment changes13 section-header bytes and makes the pinned loader additionally map192 nonzero resource-padding bytes at RVA `[0x187cb40, 0x187cc00)`, so it is not described as purely header-only at load time. Entry/imports/TLS and code content agree. No arbitrary output patch or profile approval was performed.

Reports `private/translation/steamless-format-comparison-20261112.json`, `steamless-mapped-comparison-20261112.json` and `steamless-realign-padding-20261112.json` preserve these distinctions. Missing upstream preparation provenance/reference bytes still require investigation. Do not claim an exact input match, apply unreviewed fixed-address replacements, or run the game on the strength of unpacking/PE parsing alone.

The overlay is exactly the original PE security-directory span: one structurally valid 6,448-byte `WIN_CERTIFICATE`, revision0x0200/type0x0002. Signature validity was not checked. Steamless preserves those bytes but retains the original security-directory file offset after removing `.bind`; that declared span is beyond the unpacked file's EOF. This is an output-metadata defect, not evidence that the mapped code is wrong or that either upstream hash is correct. Read-only hashes of each candidate's section-bounded byte view also matched neither upstream value. No certificate bytes or headers were repaired/removed. `private/translation/steamless-overlay-view-20261112.json` retains the inspection and unchanged-input checks.

The actual original/candidate/recipe pair passed the identity, provenance and approved-dependency checks in WSL. `local/translation/english-pair-20261112/translation-plan.json` returned exit2 with only `unpacked_hash_and_layout_not_reviewed`; the older missing-input failures remain intact. Generic read-only function discovery can proceed separately, but neither discovery nor PE parsing approves the fixed-address hooks or starts the regeneration pipeline.

## Provenance history

Skydream first added the `67a597df…` claim in [commit0a679a9, README:56](https://github.com/meizfl/skydream/blob/0a679a9dd4e11fd58a2a48b7dd8783919e5564ef/README.md#L56). That revision's generation script passes `TESV_unpacked.exe` to `write_inputs.py`, which labels the result `TESV.exe`. This supports interpreting it as a derived-input hash, but without the original private manifest it remains an inference. The `fed96638` prefix exists in [Android's initial README](https://github.com/ttom795/Skyrim_Android/blob/7e53184e78dd4a53a16df8ee05fe1ddee23531b2/README.md#L45); [the pinned Android commit](https://github.com/ttom795/Skyrim_Android/commit/4cd7686a9a4d801a4176afbdae062a2cbecbb2bb) deletes that preparation instruction without adding a replacement recipe.

Steamless changed [formatting defaults in2022](https://github.com/atom0s/Steamless/commit/12c312b4db3c056f61421601b88b30df4c6e82b9) and [Variant2.1 AES/tail-data handling in2024](https://github.com/atom0s/Steamless/commit/4c2c218a72b4eaddcf7d4c8de8cdeb20cca7c8e3). Our pin includes both changes. These establish that tool versions can change output; neither upstream project links a tool version/options to its reported hash. Older tools were not run and no mismatch explanation was asserted from those changes alone.
