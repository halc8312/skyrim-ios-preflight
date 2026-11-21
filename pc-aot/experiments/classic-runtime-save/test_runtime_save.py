"""Build private copies of the pinned runtime and run the file observation fixture."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from stage_runtime import transform, verified_source


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SAVE_PUBLISH = HERE.parent / "classic-save-publish"
COSAVE = HERE.parent / "skse-cosave"
IOS_RUNTIME = ROOT / "ios" / "runtime-port"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, default=ROOT / "vendor" / "skydream" / "runtime")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--prepared", action="store_true", help="Also apply the existing iOS runtime preparation")
    parser.add_argument("--generation", type=Path, help="Also compose the independent pinned lifted save fixture")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("This hosted fixture uses Linux file limits and section collection")
    runtime = args.runtime.resolve()
    source = runtime / "kernel32.c"
    original = source.read_bytes()
    source_text = verified_source(original)
    extra_sources = []
    feature_prefix = ""
    if args.prepared:
        sys.path.insert(0, str(IOS_RUNTIME))
        from prepare_runtime import patch_kernel
        source_text = patch_kernel(source_text)
        # The Darwin preparation prepends headers before the Linux feature macro.
        feature_prefix = "#define _GNU_SOURCE\n"
        extra_sources.append(IOS_RUNTIME / "performance_metrics.c")
    transformed = feature_prefix + transform(source_text)
    private = (ROOT / "private").resolve()
    if private != ROOT / "private":
        raise ValueError("Staged runtime source must stay in private/")
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="classic-runtime-save-", dir=private) as stage:
        out = Path(stage)
        kernel = out / "kernel32.c"
        kernel.write_text(transformed, encoding="utf-8")
        flags = ["-std=gnu11", "-DGAME_SKYRIM", "-O1", "-g", "-pthread", "-ffunction-sections", "-fdata-sections"]
        if args.sanitizers:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        variants = [("runtime-save", [], [HERE / "test_runtime_save.c", HERE / "save_attempt.c",
                                         COSAVE / "observer" / "lifecycle.c"])]
        header_runtime = runtime
        if args.generation is not None:
            sys.path.insert(0, str(SAVE_PUBLISH))
            from test_save_publish import HEADERS, lifted_fixture
            header_runtime = args.generation.resolve() / "source" / "runtime"
            for header, expected in HEADERS.items():
                data = (header_runtime / header).read_bytes()
                if hashlib.sha256(data).hexdigest() != expected:
                    raise ValueError("Runtime header differs from the pinned lifted ABI")
            for header in ("rt.h", "game.h", "host_compat.h", "host.h"):
                baseline = (runtime / header).read_bytes().replace(b"\r\n", b"\n")
                if (header_runtime / header).read_bytes() != baseline:
                    raise ValueError("Runtime header differs from the fixed vendor API")
            lifted = out / "lifted.c"
            lifted.write_bytes(lifted_fixture(args.generation))
            variants.append(("lifted-save", ["-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1", "-DSKYRIM_TEST_FILE_RUNTIME=1"],
                             [lifted, SAVE_PUBLISH / "test_save_publish.c", HERE / "lifted_backend.c",
                              COSAVE / "observer" / "lifecycle.c"]))
        includes = ["-isystem", header_runtime, "-I", HERE, "-I", SAVE_PUBLISH, "-I", COSAVE, "-I", IOS_RUNTIME]
        kernel_object = out / "kernel32.o"
        subprocess.run([args.cc, *flags, *includes, "-c", kernel, "-o", kernel_object], check=True, timeout=60)
        common = [kernel_object, HERE / "runtime_save.c", HERE / "host_traps.c", SAVE_PUBLISH / "save_publish.c"]
        for name, definitions, sources in variants:
            binary = out / name
            subprocess.run([args.cc, *flags, *definitions, "-Wall", "-Wextra", "-Werror", *includes,
                            *common, *sources, *extra_sources, "-Wl,--gc-sections", "-o", binary],
                           check=True, timeout=60)
            with tempfile.TemporaryDirectory(prefix="classic-runtime-save-data-") as data:
                subprocess.run([binary, data], check=True, timeout=20)
    if source.read_bytes() != original:
        raise AssertionError("Original runtime source changed")


if __name__ == "__main__":
    main()
