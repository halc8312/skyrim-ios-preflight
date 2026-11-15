# Unlinked performance experiments

These authored experiments are not included in the iOS app or dependency patches.
They are inputs to a decision, not shipping optimizations or iPhone benchmarks.

- `mvk-module-cold/`: module-local cold shader generation on the pinned build7 base, with diagnostic-only converter exclusion and shared compiler-error publication protection. The revised method fixture passes ASan/UBSan and TSan; a separate real SPIRV-Cross/MoltenVK converter check matches 24 conditions over 512 comparisons with and without serialization. Apple Metal and iPhone effects remain unverified. See its README before considering app integration.

- `dds-view/dds_view.h`: logical DDS header plus borrowed payload. Caller owns immutable storage until synchronous consumption completes. Any asynchronous upload must independently own staging. Only complete power-of-two compressed 2D DDS inputs are admitted; unsupported inputs are refused before publication. No game allocator, D3DX creation/lock/unlock, or guest ABI is replaced.
- `mvk-flight/`: per-key pending macro-library compilation prototype. The test injects it into the pinned public methods at the existing lookup boundary. Nested compile uses independent compilation to avoid self-wait. Failed library objects stay cached; exceptions release pending records and wake followers. Concurrent destruction/assignment is not supported. The condition wait lacks a proven Apple deadline/transitive-lock contract, so it must not be copied into the production adapter yet.

Local POSIX fixture commands:

```sh
python3 -m unittest discover -s tests -p test_dds_view.py -v
python3 -m unittest discover -s tests -p test_mvk_flight.py -v
```

The macro fixture needs the same `SKYRIM_MVK_GPU_SOURCE`, `SKYRIM_MVK_CONVERTER_SOURCE`, and `SKYRIM_MVK_CODEC_SOURCE` pinned public inputs as `test_mvk_library.py`. Set `SKYRIM_TEST_SANITIZERS=1` for ASan/UBSan. Test executables have external deadlines; that is not a production timeout implementation. No private game inputs or Apple build are required.

Before adoption, resolve the boundaries in [NATIVE_MIGRATION.md](../docs/NATIVE_MIGRATION.md) and [REVERSE_ENGINEERING_PLAN.md](../docs/REVERSE_ENGINEERING_PLAN.md). Authored byte equality is not an actual GPU pixel comparison; a compile-count reduction with fake Metal is not a frame-time improvement.
