"""Build private copies of the pinned runtime and run the file observation fixture."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from stage_runtime import transform, verified_source


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SAVE_PUBLISH = HERE.parent / "classic-save-publish"
FRESH_LOAD = HERE.parent / "classic-fresh-load"
COSAVE = HERE.parent / "skse-cosave"
IOS_RUNTIME = ROOT / "ios" / "runtime-port"


def crt_fixture(generation: Path) -> bytes:
    sys.path.insert(0, str(FRESH_LOAD))
    from test_slot_read import (
        READ_CALL, READER_SHA, READER_UNIT, SLOT_UNIT, check_unit, fixture_function,
        function_span, stage_slot_read, validate_generation,
    )
    state = json.loads((generation / "generation.json").read_bytes())
    validate_generation(state)
    image = Path(state["candidate"]).read_bytes()
    if hashlib.sha256(image).hexdigest() != state["candidate_sha256"]:
        raise ValueError("Pinned vtable comparison needs the original verified image")
    pe, = struct.unpack_from("<I", image, 60)
    sections, = struct.unpack_from("<H", image, pe + 6)
    optional_size, = struct.unpack_from("<H", image, pe + 20)
    for section in range(sections):
        entry = pe + 24 + optional_size + section * 40
        _, rva, raw_size, offset = struct.unpack_from("<4I", image, entry + 8)
        slot = 0x110DBEC - (rva + 0x400000)
        if 0 <= slot <= raw_size - 4 and struct.unpack_from("<I", image, offset + slot) == (0xA57E70,):
            break
    else:
        raise ValueError("Pinned BSFile read slot differs from the reviewed input")

    root = generation / "gen"
    units = (
        (READER_UNIT, READER_SHA, ("f_677990",)),
        ("exe_261.c", "1f4ea50599764ad19c3bf5752598ab2036fb4b6fd8d7802774a288ab903331b5",
         ("f_A57260", "f_A57E70")),
        ("exe_295.c", "0efcfed401f53e7f1a6e05d69664975e95949895a0795094d063a03cba865404", ("f_B29A10",)),
        ("exe_470.c", "f9498a5a82f02b1886e638c867eac04f83501f340f4fe13a52511c95eff0a3f7", ("f_F680D9",)),
    )
    _, read_block, _ = stage_slot_read((root / SLOT_UNIT).read_bytes())
    wrapped = read_block.replace(READ_CALL, b"""{
        const uint32_t stream = c->r[1];
        const uint32_t frame = classic_runtime_load_slot_read_begin(c, stream);
        f_677990(c);
        classic_runtime_fixture_slot_result(classic_runtime_load_slot_read_end(c, stream, frame, c->r[0]));
    }""", 1)
    sources = [b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n#include "runtime_load.h"\n'
               b'void classic_runtime_fixture_slot_result(uint64_t);\n']
    for name, digest, functions in units:
        data = (root / name).read_bytes()
        check_unit(data, digest)
        for function in functions:
            begin, end = function_span(data, function)
            sources.append(data[begin:end])
    sources.append(fixture_function("classic_runtime_fixture_crt_read", wrapped))
    return b"\n".join(sources)



def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, default=ROOT / "vendor" / "skydream" / "runtime")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--prepared", action="store_true", help="Also apply the existing iOS runtime preparation")
    parser.add_argument("--generation", type=Path, help="Also compose the independent pinned lifted save fixture")
    parser.add_argument("--load-witness", action="store_true", help="Also stage the read-only selected-file witness")
    parser.add_argument("--load-clone", action="store_true", help="Also run the opt-in immutable GFile fixture")
    args = parser.parse_args()
    args.load_witness |= args.load_clone
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
    transformed = feature_prefix + transform(source_text, load_witness=args.load_witness)
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
                                         COSAVE / "event_trace.c", COSAVE / "observer" / "lifecycle.c"]),
                    ("selected-load-path", [], [FRESH_LOAD / "test_selected_path.c"] +
                     ([] if args.load_witness else [FRESH_LOAD / "selected_path.c"]))]
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
        if args.load_witness:
            includes += ["-I", FRESH_LOAD]
            variants.append(("selected-load-witness", [], [HERE / "test_runtime_load.c"]))
            if args.generation is not None:
                includes += ["-isystem", args.generation / "gen"]
                linked_crt = out / "crt_read.c"
                linked_crt.write_bytes(crt_fixture(args.generation))
                variants.append(("selected-load-crt", ["-DSKYRIM_TEST_PINNED_CRT=1"],
                                 [HERE / "test_runtime_load.c", linked_crt]))
        kernel_object = out / "kernel32.o"
        subprocess.run([args.cc, *flags, *includes, "-c", kernel, "-o", kernel_object], check=True, timeout=60)
        common = [kernel_object, HERE / "runtime_save.c", HERE / "host_traps.c", SAVE_PUBLISH / "save_publish.c"]
        if args.load_witness:
            common += [HERE / "runtime_load.c", FRESH_LOAD / "selected_path.c"]
        for name, definitions, sources in variants:
            binary = out / name
            subprocess.run([args.cc, *flags, *definitions, "-Wall", "-Wextra", "-Werror", *includes,
                            *common, *sources, *extra_sources, "-Wl,--gc-sections", "-o", binary],
                           check=True, timeout=60)
            with tempfile.TemporaryDirectory(prefix="classic-runtime-save-data-") as data:
                subprocess.run([binary, data], check=True, timeout=20)
        if args.load_clone:
            cloned_kernel = out / "kernel32-clone.c"
            cloned_kernel.write_text(feature_prefix + transform(source_text, load_witness=True, load_clone=True),
                                     encoding="utf-8")
            definition = "-DSKYRIM_EXPERIMENT_CLONE_LOAD"
            clone_object = out / "kernel32-clone.o"
            subprocess.run([args.cc, *flags, definition, *includes, "-c", cloned_kernel, "-o", clone_object],
                           check=True, timeout=60)
            bridge_sources = [COSAVE / name for name in (
                "event_bridge.c", "generation.c", "digest.c", "immutable_ess.c", "records.c", "observer/lifecycle.c",
            )]
            binary = out / "selected-load-clone"
            subprocess.run([args.cc, *flags, definition, "-DSKSE_TEST_NAMED_CLONE", "-Wall", "-Wextra", "-Werror",
                            *includes, clone_object, *common[1:], HERE / "test_runtime_load_clone.c",
                            *bridge_sources, *extra_sources, "-Wl,--gc-sections", "-lsqlite3", "-lcrypto", "-o", binary],
                           check=True, timeout=60)
            with tempfile.TemporaryDirectory(prefix="classic-runtime-clone-data-") as data:
                subprocess.run([binary, data], check=True, timeout=20)
    if source.read_bytes() != original:
        raise AssertionError("Original runtime source changed")


if __name__ == "__main__":
    main()
