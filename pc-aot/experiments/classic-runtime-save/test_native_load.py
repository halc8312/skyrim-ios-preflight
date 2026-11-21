"""Compose the verified native snapshot and original lifted selected-load boundaries."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_runtime_save import COSAVE, FRESH_LOAD, HERE, IOS_RUNTIME, ROOT, SAVE_PUBLISH, crt_fixture
from stage_runtime import transform, verified_source


def stage_fixture(units: dict[str, bytes]) -> bytes:
    from stage_native_load import UNIT, VM_UNIT, VM_REVERT, function_span, vm_call
    from test_slot_read import fixture_function

    header = (b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n#include "native_load.h"\n'
              b'void skyrim_vm_host_before_revert(unsigned);\nvoid skyrim_vm_host_after_load(int);\n')
    functions = [(UNIT, "f_6821C0"), (VM_UNIT, "f_8D6990"), (VM_UNIT, "f_8D69C0"), (VM_UNIT, "f_8D6C80")]
    bodies = []
    for unit, name in functions:
        start, end = function_span(units[unit], name)
        bodies.append(units[unit][start:end])
    return header + b"\n".join(bodies) + fixture_function("classic_runtime_fixture_revert", vm_call(VM_REVERT))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("The hosted fixture uses Linux and an authored clone operation")
    generation = args.generation.resolve()
    runtime = generation / "source" / "runtime"
    sys.path[:0] = [str(FRESH_LOAD), str(IOS_RUNTIME)]
    from test_fresh_load import HEADERS
    from stage_native_load import stage_native_load
    from prepare_runtime import patch_kernel

    for filename, digest in HEADERS.items():
        if hashlib.sha256((runtime / filename).read_bytes()).hexdigest() != digest:
            raise ValueError("Prepared runtime header differs from the pinned guest ABI")
    original = runtime / "kernel32.c"
    kernel_source = original.read_bytes()
    private = (ROOT / "private").resolve()
    if private != ROOT / "private":
        raise ValueError("Generated test sources must stay under private/")
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="classic-native-load-", dir=private) as scratch:
        out = Path(scratch)
        kernel, lifted, crt = (out / name for name in ("kernel32.c", "load.c", "crt.c"))
        kernel.write_text("#define _GNU_SOURCE\n" + transform(patch_kernel(verified_source(kernel_source)),
                                                             load_witness=True, load_clone=True), encoding="utf-8")
        staged = stage_native_load(generation)
        originals = {name: (generation / "gen" / name).read_bytes() for name in staged}
        lifted.write_bytes(stage_fixture(staged))
        crt.write_bytes(crt_fixture(generation, native=True))
        flags = ["-std=gnu11", "-DGAME_SKYRIM", "-O1", "-g", "-pthread", "-ffunction-sections", "-fdata-sections",
                 "-DSKYRIM_EXPERIMENT_CLONE_LOAD", "-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1"]
        if args.sanitizers:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        includes = [option for folder in (runtime, generation / "gen") for option in ("-isystem", folder)]
        includes += [part for folder in (HERE, FRESH_LOAD, COSAVE, IOS_RUNTIME, SAVE_PUBLISH) for part in ("-I", folder)]
        for name, unit in staged.items():
            full = out / name
            full.write_bytes(unit)
            subprocess.run([args.cc, *flags, *includes, "-w", "-fsyntax-only", full], check=True, timeout=90)
        under_test = [HERE / file for file in ("native_load.c", "runtime_load.c", "runtime_save.c", "host_traps.c")]
        under_test += [FRESH_LOAD / "fresh_load.c", FRESH_LOAD / "selected_path.c",
                       IOS_RUNTIME / "performance_metrics.c", SAVE_PUBLISH / "save_publish.c"]
        under_test += [COSAVE / file for file in ("event_bridge.c", "event_trace.c", "generation.c", "digest.c",
                                                 "immutable_ess.c", "records.c", "observer/lifecycle.c")]
        kernel_object = out / "kernel32.o"
        subprocess.run([args.cc, *flags, *includes, "-c", kernel, "-o", kernel_object], check=True, timeout=90)
        binary = out / "native-load"
        subprocess.run([args.cc, *flags, *includes, "-DSKSE_TEST_NAMED_CLONE", "-Wall", "-Wextra", "-Werror",
                        kernel_object, lifted, crt, HERE / "test_native_load.c", *under_test,
                        "-Wl,--gc-sections", "-lsqlite3", "-lcrypto", "-o", binary], check=True, timeout=150)
        with tempfile.TemporaryDirectory(prefix="classic-native-load-data-") as data:
            subprocess.run([binary, data], check=True, timeout=20)
        if any((generation / "gen" / name).read_bytes() != unit for name, unit in originals.items()):
            raise AssertionError("The original fixed load generation changed")
    if original.read_bytes() != kernel_source:
        raise AssertionError("The original fixed runtime changed")


if __name__ == "__main__":
    main()
