"""Exercise the reviewed fresh-save publication calls with an authored file backend."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
COSAVE = HERE.parent / "skse-cosave"
sys.path[:0] = [str(HERE.parent / name) for name in ("classic-fresh-load", "classic-runtime-save")]
from test_fresh_load import HEADERS, BOOL_RESULT, at_site, instrument_lifecycle
from stage import check_unit, function_span, validate_generation
from stage_save_notifications import CLOSE_CALL, FLUSH_CALL, MOVE, SLOT_CALL, UNITS, VM_CALL


BUFFER_UNIT = "0efcfed401f53e7f1a6e05d69664975e95949895a0795094d063a03cba865404"
MOVED = (b"    esp -= 4; M32(esp) = 0x68031Cu; SPILL();\n"
         b"    fixture_final_move_enter(c);\n"
         b"    f_F53F06(c);\n"
         b"    fixture_final_move_leave(c);\n"
         b"    RELOAD();\n")
CALLEES = (
    "f_680040", "f_F562C2", "f_677A40", "f_677770", "f_677540", "f_680F60", "f_860F00",
    "f_67E020", "f_679200", "f_680190", "f_685790", "f_67E4C0", "f_680D30", "f_680660",
    "f_F52394", "f_677520", "f_A487B0", "f_F5372D", "f_F53F06", "f_F53EE3", "f_8CD8F0",
    "f_B29850", "f_F56351", "f_A58000", "f_B29950", "fixture_flush_result", "fixture_close_result",
    "fixture_final_move_enter", "fixture_final_move_leave",
)


def original(unit: bytes, name: str) -> bytes:
    begin, end = function_span(unit, name)
    return unit[begin:end]


def witness_result(body: bytes, call: bytes, witness: bytes) -> bytes:
    before = call.removesuffix(b" RELOAD();\n")
    return at_site(body, call, before + b"\n    " + witness + b"(c);\n    RELOAD();\n")


def lifted_fixture(generation: Path, *, buffered: bool = False) -> bytes:
    state = json.loads((generation / "generation.json").read_bytes())
    validate_generation(state)
    if (state.get("mode") != "original-guest-bodies" or state.get("fixed_native_replacements") is not False or
            state.get("generation_finished") is not True or state.get("inputs_unchanged") is not True):
        raise ValueError("Requires a completed, unchanged original-body generation")
    units = {}
    for filename, expected in UNITS.items():
        units[filename] = (generation / "gen" / filename).read_bytes()
        check_unit(units[filename], expected)

    body = original(units["exe_103.c"], "f_6814D0")
    head = body[:body.index(b"L_6814D0:\n")]
    # The authored frame enters the original branch for manager + 0x30 == 0.
    tail = body[body.index(b"L_68161A:\n"):]
    save = head + b"    goto L_68161A;\n" + instrument_lifecycle(
        tail, SLOT_CALL, b"f_679200(c)", b"SkyrimSaveSlot", b"SkyrimNoResult",
    )
    close_save = at_site(original(units["exe_102.c"], "f_680190"), MOVE, MOVED)
    crt_move = original(units["exe_467.c"], "f_F53F06")
    close_stream = original(units["exe_101.c"], "f_677520")
    destructor = original(units["exe_261.c"], "f_A579B0")
    destructor = witness_result(destructor, FLUSH_CALL, b"fixture_flush_result")
    destructor = witness_result(destructor, CLOSE_CALL, b"fixture_close_result")

    body = original(units["exe_198.c"], "f_8D3750")
    head = body[:body.index(b"L_8D3750:\n")]
    block = body[body.index(b"L_8D3849:\n"):body.index(b"L_8D385A:\n")]
    vm = head + b"    goto L_8D3849;\n" + instrument_lifecycle(
        block, VM_CALL, b"f_8CD8F0(c)", b"SkyrimSaveVM", BOOL_RESULT,
    ) + b"L_8D385A:\nL_8D38D2:\n    SPILL(); return;\n}\n"

    writers = b""
    callees = CALLEES
    if buffered:
        buffer = (generation / "gen" / "exe_295.c").read_bytes()
        check_unit(buffer, BUFFER_UNIT)
        writers = (original(units["exe_101.c"], "f_6779D0") + original(units["exe_261.c"], "f_A57280") +
                   original(units["exe_261.c"], "f_A57240") + original(buffer, "f_B29AD0") +
                   original(buffer, "f_B29850"))
        callees += ("f_B29AD0", "f_F52E30", "f_F57FB6")
    prototypes = b"".join(f"void {name}(CPU *);\n".encode("ascii") for name in callees)
    return (b'#include "lift_prelude.h"\n#include "observer/lifecycle.h"\n' + prototypes + save +
            close_save + crt_move + close_stream + destructor + vm + writers)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--ldflag", action="append", default=[])
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--compose", action="store_true", help="Test synchronous companion capture on Linux")
    args = parser.parse_args()
    if args.compose and not sys.platform.startswith("linux"):
        parser.error("--compose is supported on Linux")
    generation = args.generation
    private = (ROOT / "private").resolve()
    if private != ROOT / "private":
        raise ValueError("The hosted fixture must stay in private/")
    private.mkdir(exist_ok=True)
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1"]
    if args.sanitizers:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    extra_sources = []
    libraries = []
    if args.compose:
        flags += ["-DSKYRIM_TEST_SAVE_COSAVE=1", "-ffunction-sections", "-fdata-sections"]
        extra_sources = [HERE / "cosave_fixture.c", *(COSAVE / name for name in (
            "event_bridge.c", "event_trace.c", "generation.c", "immutable_ess.c", "digest.c", "records.c",
        ))]
        libraries = ["-Wl,--gc-sections", "-lsqlite3", "-lm", "-lcrypto"]
    with tempfile.TemporaryDirectory(prefix="classic-save-publish-", dir=private) as stage:
        out = Path(stage)
        headers = out / "vendor"
        headers.mkdir()
        for filename, expected in HEADERS.items():
            data = (generation / "source" / "runtime" / filename).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError("Generated runtime header identity changed")
            (headers / filename).write_bytes(data)
        lifted = out / "lifted.c"
        lifted.write_bytes(lifted_fixture(generation))
        binary = out / "save-publish"
        subprocess.run([args.cc, *flags, *args.cflag, "-isystem", headers, "-I", HERE, "-I", COSAVE,
                        lifted, HERE / "save_publish.c", HERE / "test_save_publish.c",
                        COSAVE / "observer/lifecycle.c", *extra_sources,
                        *args.ldflag, *libraries, "-o", binary], check=True, timeout=60)
        with tempfile.TemporaryDirectory(prefix="classic-save-publish-data-") as data:
            subprocess.run([binary, data], check=True, timeout=20)


if __name__ == "__main__":
    main()
