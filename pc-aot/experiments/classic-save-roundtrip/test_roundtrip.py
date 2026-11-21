"""Run the pinned real-wrapper save and a fresh lifted load against one store."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RUNTIME = HERE.parent / "classic-runtime-save"
SAVE = HERE.parent / "classic-save-publish"
LOAD = HERE.parent / "classic-fresh-load"
COSAVE = HERE.parent / "skse-cosave"
PROJECTION = HERE.parent / "classic-form-projection"
PACKAGE = HERE.parent / "classic-package-decision"
IOS = ROOT / "ios" / "runtime-port"
sys.path[:0] = [str(path) for path in (RUNTIME, SAVE, LOAD, IOS, PROJECTION)]

from stage_runtime import transform, verified_source
from prepare_runtime import patch_kernel
from test_save_publish import HEADERS, lifted_fixture as lifted_save
from test_fresh_load import lifted_fixture as lifted_load
from test_form_projection import stage_lift as lifted_projection


CASES = (
    ("committed", "matched"), ("vm-rejected", "missing"), ("short-write", "missing"),
    ("stage-error", "missing"), ("clone-unavailable", "missing"), ("interrupted", "missing"),
    ("replaced-before-capture", "missing"), ("replaced", "replaced"),
)


def runtime_headers(generation: Path, vendor: Path) -> Path:
    headers = generation / "source" / "runtime"
    for name, expected in HEADERS.items():
        if hashlib.sha256((headers / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Runtime header differs from the pinned lifted ABI")
    for name in ("rt.h", "game.h", "host_compat.h", "host.h"):
        if (headers / name).read_bytes() != (vendor / name).read_bytes().replace(b"\r\n", b"\n"):
            raise ValueError("Runtime header differs from the fixed vendor API")
    return headers


def fixture_copy(path: Path, substitutions: tuple[tuple[str, str], ...]) -> str:
    text = path.read_text(encoding="utf-8")
    for before, after in substitutions:
        if text.count(before) != 1:
            raise ValueError("Expected one hosted file-capture call site")
        text = text.replace(before, after, 1)
    return text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, default=ROOT / "vendor" / "skydream" / "runtime")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--ldflag", action="append", default=[])
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--project-actor", action="store_true",
                        help="also test a copied actor from the verified load")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("This hosted fixture requires Linux memfd seals")
    private = (ROOT / "private").resolve()
    if private != ROOT / "private":
        raise ValueError("Staged sources must stay in private/")
    private.mkdir(exist_ok=True)

    source = args.runtime / "kernel32.c"
    original = source.read_bytes()
    headers = runtime_headers(args.generation, args.runtime)
    flags = ["-std=gnu11", "-DGAME_SKYRIM", "-O1", "-g", "-pthread", "-ffunction-sections", "-fdata-sections"]
    if args.sanitizers:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    includes = ["-isystem", headers]
    for path in (HERE, RUNTIME, SAVE, LOAD, COSAVE, IOS, PACKAGE):
        includes.extend(("-I", path))

    with tempfile.TemporaryDirectory(prefix="classic-save-roundtrip-", dir=private) as stage:
        out = Path(stage)
        kernel, save, load = (out / name for name in ("kernel32.c", "save.c", "load.c"))
        kernel.write_text("#define _GNU_SOURCE\n" + transform(patch_kernel(verified_source(original))), encoding="utf-8")
        save.write_bytes(lifted_save(args.generation))
        _, load_lift = lifted_load(args.generation, True)
        load.write_bytes(load_lift)

        def compile_object(path: Path, definitions: list[str]) -> Path:
            output = out / (path.stem + ".o")
            subprocess.run([args.cc, *flags, *args.cflag, *definitions, *includes, "-c", path, "-o", output],
                           check=True, timeout=60)
            return output

        kernel_object = compile_object(kernel, [])
        capture = out / "immutable_ess.c"
        capture.write_text(fixture_copy(COSAVE / "immutable_ess.c", (
            ('#include "immutable_ess.h"', '#include "immutable_ess.h"\n#include "roundtrip.h"'),
            ('openat(dir, ".", O_TMPFILE | O_EXCL | O_RDWR | O_CLOEXEC, 0600)',
             'roundtrip_openat(dir, ".", O_TMPFILE | O_EXCL | O_RDWR | O_CLOEXEC, 0600)'),
            ("ioctl(writer, FICLONE, source)", "roundtrip_ioctl(writer, FICLONE, source)"),
        )), encoding="utf-8")
        hashes = out / "digest.c"
        hashes.write_text(fixture_copy(COSAVE / "digest.c", (
            ('#include "digest.h"', '#include "digest.h"\n#include "roundtrip.h"'),
            ("pread(fd, buffer, size, offset)", "roundtrip_digest_pread(fd, buffer, size, offset)"),
        )), encoding="utf-8")
        immutable = compile_object(capture, [])
        digest = compile_object(hashes, [])
        shared_sources = [HERE / "fixture_io.c", *(COSAVE / name for name in (
            "event_bridge.c", "event_trace.c", "generation.c", "records.c", "observer/lifecycle.c",
        ))]
        common = [immutable, digest, *(compile_object(path, ["-Wall", "-Wextra", "-Werror"]) for path in shared_sources)]

        def build(name: str, definitions: list[str], files: list[Path]) -> Path:
            output = out / name
            subprocess.run([args.cc, *flags, *args.cflag, "-Wall", "-Wextra", "-Werror",
                            "-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1", *definitions, *includes, *common, *files,
                            *args.ldflag, "-Wl,--gc-sections", "-lsqlite3", "-lm", "-lcrypto", "-o", output],
                           check=True, timeout=120)
            return output

        saver = build("save", ["-DSKYRIM_TEST_FILE_RUNTIME=1", "-DSKYRIM_TEST_SAVE_COSAVE=1",
                               "-DSKYRIM_TEST_SAVE_ROUNDTRIP=1"],
                      [kernel_object, save, SAVE / "test_save_publish.c", SAVE / "save_publish.c",
                       HERE / "save_phase.c", IOS / "performance_metrics.c",
                       RUNTIME / "runtime_save.c", RUNTIME / "lifted_backend.c", RUNTIME / "host_traps.c"])
        load_flags = ["-DSKYRIM_TEST_FRESH_COSAVE=1", "-DSKYRIM_TEST_LOAD_ROUNDTRIP=1"]
        load_sources = [load, LOAD / "test_fresh_load.c", LOAD / "fresh_load.c", LOAD / "cosave_fixture.c"]
        if args.project_actor:
            projection = out / "projection.c"
            projection.write_bytes(lifted_projection(args.generation, roundtrip=True))
            managed = out / "managed.c"
            managed.write_bytes(lifted_projection(args.generation, roundtrip=True, managed=True))
            form_view = PACKAGE / "form_view.c"
            managed_view = compile_object(form_view, [
                "-Dclassic_engine_read_data=classic_managed_actor_read_data",
                "-Dclassic_read_form_id=classic_managed_actor_read_form_id",
            ])
            load_flags.append("-DSKYRIM_TEST_PROJECT_FORM=1")
            load_sources.extend((projection, PROJECTION / "test_form_projection.c", form_view,
                                 managed, PROJECTION / "test_managed_actor.c", managed_view))
        loader = build("load", load_flags, load_sources)
        for save_case, load_case in CASES:
            with tempfile.TemporaryDirectory(prefix="classic-roundtrip-data-") as data:
                scope = Path(data)
                subprocess.run([saver, scope, save_case], check=True, timeout=20)
                profile = scope / "saved-profile"
                subprocess.run([loader, profile, load_case], check=True, timeout=20)
                if load_case == "matched":
                    subprocess.run([loader, profile, "preloaded"], check=True, timeout=20)
    if source.read_bytes() != original:
        raise AssertionError("Original runtime source changed")
    print("PASS eight persisted save-to-load outcomes and cached preload exclusion")


if __name__ == "__main__":
    main()
