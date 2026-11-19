# Classic save and load lifecycle observation

This local experiment matches seven calls in official SKSE 1.7.3 with a user-owned Skyrim 1.9.32 executable, its discovery graph and its generated C. The original SDK files are checked by SHA-256. The tool checks the image and graph against the completed generation manifest, verifies each on-disk direct callee and matches the return address and caller in the generated function. The game and its generated code stay in the owner's private workspace.

From `pc-aot`, run the tests with `python3 tests/test_classic_lifecycle.py -v`. To check a local generation, point the following arguments at the original inputs.

```sh
python3 experiments/classic-lifecycle/prepare.py \
  --official "$CLASSIC_SDK_SOURCE" --image "$OWNED_GAME_IMAGE" \
  --discovery "$PRIVATE_DISCOVERY" --generation "$PRIVATE_GENERATION_MANIFEST" \
  --generated "$PRIVATE_GENERATED_C"
```

The three output integers give the verified call count, the number of affected source files and whether copies were written. `--private-output private/new-directory` writes only newly instrumented copies into a fresh directory. For an opt-in build, compile those copies in place of the corresponding generated units, add this directory to the include path, compile `lifecycle.c` and define `SKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1` for the copied units. The ordinary app build uses its original generated sources.

| Event | Call boundary |
| --- | --- |
| 1 | Save slot management |
| 2 | Load slot management |
| 3 | Save VM data |
| 4 | Load VM data |
| 5 | Revert VM data on game load |
| 6 | Revert VM data on script reload |
| 7 | Register Papyrus functions |

`skyrim_lifecycle_snapshot` returns up to the first 256 events since process start, plus the count lost to capacity. Each row contains five integers. They identify the event, entry (0) or return (1), outcome, call ticket and same-thread parent ticket. A parent of 0 starts an independent trace. Outcome 0 means no result was collected, 1 means the guest returned false and 2 means it returned true. Only events 2, 3 and 4 report the low byte of the original return register. Reading again after the callers finish covers any concurrently reserved rows. The observer never opens a save or reads a guest filename, handle, argument or memory buffer.

A nonlocal guest exit can leave an entry without its corresponding return. Its ticket and subsequent same-thread links stay provisional. A full buffer can also omit returns. Only interpret nesting for paired calls in a complete, quiescent trace.

The file named `synthetic.c` exercises nested success and failure with an authored one-byte format and a temporary profile copy. Runtime coverage, ordering across game threads, co-save publication and callback compatibility each need separate evidence from the actual game.
