"""Run the reviewed fresh selected-load call chain with authored file services."""

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
sys.path.insert(0, str(HERE.parent / "classic-package-decision"))
from stage import check_unit, function_span, validate_generation


UNIT = "exe_103.c"
SHA256 = "f6e3c6c49403e7a00b5da4f30f436123c943254ae39eae4288885f75313c4fd7"
ENTRY = b"L_6821C0:\n"
CACHE = b"L_6821EC:\n    esi = (uint32_t)(TL32((uint32_t)(edi + 0x30u)));\n"
OPEN = b"    esp -= 4; M32(esp) = 0x682225u; SPILL(); f_677540(c); RELOAD();\n"
SLOT = b"    esp -= 4; M32(esp) = 0x68227Au; SPILL(); f_67B720(c); RELOAD();\n"
TERMINAL = b"    esp += 20; SPILL(); return;\n"
REPLACEMENTS = (
    (ENTRY, ENTRY + b"    ticket = classic_fresh_enter(c);\n"),
    (CACHE, CACHE + b"    if (esi) classic_fresh_cached(ticket, c);\n"),
    (OPEN, b"    esp -= 4; M32(esp) = 0x682225u; SPILL();\n"
           b"    classic_fresh_open_enter(ticket, c, esi, esp + 0x14u);\n"
           b"    f_677540(c);\n"
           b"    classic_fresh_open_leave(ticket, c, (uint8_t)c->r[0] != 0);\n"
           b"    RELOAD();\n"),
    (SLOT, b"    esp -= 4; M32(esp) = 0x68227Au; SPILL();\n"
           b"    classic_fresh_slot_enter(ticket, c, esi);\n"
           b"    f_67B720(c);\n"
           b"    classic_fresh_slot_leave(ticket, c, (uint8_t)c->r[0] != 0);\n"
           b"    RELOAD();\n"),
    (TERMINAL, b"    esp += 20; SPILL(); classic_fresh_leave(ticket, c, (uint8_t)eax != 0); return;\n"),
)
DECLARATION = b"void f_6821C0(CPU *c) {\n"
TICKET = b"    uint64_t ticket;\n"
INCLUDE = b'#include "fresh_load.h"\n'
HEADERS = {
    "x86.h": "dcc654965668c5cbe3f8aaa74cd1f8fd0bb53312e8de1747e6938ffc6cd03793",
    "lift_prelude.h": "59cd9f85d2a5905c0c04dd1b5c71de0443e51dad06d19c09fc69259e6637a746",
    "rt_table.h": "eba5a14664b66a8eccfa901f289d80e6155dac0df5e72a84f24a5f641656a612",
}
CALLEES = (
    "f_F52394", "f_680040", "f_677A40", "f_677770", "f_677540", "f_6820C0",
    "f_67B720", "f_681E00", "f_87AC60", "f_680190", "f_685790", "f_680660", "f_6928A0",
)
VM_UNIT = "exe_199.c"
VM_SHA256 = "f550390f7023b2375da3746cfd6b99be32bfd4fecabff2ce5396b73e2e18bf3c"
LIFECYCLE_INCLUDE = b'#include "observer/lifecycle.h"\n'
SLOT_INVOKE = b"    f_67B720(c);\n"
REVERT_CALL = b"    esp -= 4; M32(esp) = 0x8D69ACu; SPILL(); f_8D5DF0(c); RELOAD();\n"
LOAD_VM_CALL = b"    esp -= 4; M32(esp) = 0x8D6B7Eu; SPILL(); f_8D3DB0(c); RELOAD();\n"
VM_CALLEES = ("f_C398A0", "f_8D5DF0", "f_401710", "f_8B9AA0", "f_8D3DB0")
BOOL_RESULT = b"((uint8_t)c->r[0] ? SkyrimReturnedTrue : SkyrimReturnedFalse)"


def at_site(body: bytes, anchor: bytes, updated: bytes) -> bytes:
    if body.count(anchor) != 1:
        raise ValueError("Expected exactly one selected-load site")
    return body.replace(anchor, updated, 1)


def lifecycle_call(site: bytes, invocation: bytes, result: bytes) -> bytes:
    return b"    SKYRIM_LIFECYCLE_CALL(" + site + b", " + invocation + b",\n                          " + result + b");\n"


def instrument_lifecycle(body: bytes, anchor: bytes, invocation: bytes, site: bytes, result: bytes) -> bytes:
    prologue, tail = anchor.split(b"SPILL(); ")
    if tail != invocation + b"; RELOAD();\n":
        raise ValueError("Expected a lifted call with the original spill/reload")
    return at_site(body, anchor,
                   prologue + b"SPILL();\n" + lifecycle_call(site, invocation, result) + b"    RELOAD();\n")


def vm_fixture(unit: bytes) -> bytes:
    check_unit(unit, VM_SHA256)
    start, end = function_span(unit, "f_8D6990")
    revert = instrument_lifecycle(unit[start:end], REVERT_CALL, b"f_8D5DF0(c)",
                                  b"SkyrimRevertForLoad", b"SkyrimNoResult")
    start, end = function_span(unit, "f_8D69C0")
    body = unit[start:end]
    prefix = body[:body.index(b"L_8D69C0:\n")]
    first = b"L_8D6B71:\n"
    last = b"L_8D6B88:\n"
    if body.count(first) != 1 or body.count(last) != 1:
        raise ValueError("Expected one VM dispatch block")
    block = body[body.index(first):body.index(last) + len(last)]
    load = instrument_lifecycle(block, LOAD_VM_CALL, b"f_8D3DB0(c)", b"SkyrimLoadVM", BOOL_RESULT)
    # Start at the reviewed dispatch block and stop after its original result branch.
    return revert + prefix + b"    goto L_8D6B71;\n" + load + b"    SPILL();\n}\n"


def transform(unit: bytes) -> bytes:
    start, end = function_span(unit, "f_6821C0")
    body = unit[start:end]
    check_unit(unit, SHA256)
    for anchor, updated in REPLACEMENTS:
        body = at_site(body, anchor, updated)
    body = body.replace(DECLARATION, DECLARATION + TICKET, 1)
    return INCLUDE + unit[:start] + body + unit[end:]


def check_stage(source: Path, before: bytes, after: bytes) -> None:
    recovered = after.removeprefix(INCLUDE).replace(DECLARATION + TICKET, DECLARATION, 1)
    for anchor, updated in REPLACEMENTS:
        recovered = recovered.replace(updated, anchor, 1)
    if recovered != before or source.read_bytes() != before:
        raise AssertionError("Source changes escaped the selected-load sites")
    for bad in (before.replace(OPEN, b"", 1), before + b"\n", after):
        try:
            transform(bad)
        except ValueError:
            continue
        raise AssertionError("Changed generation was accepted")


def lifted_fixture(generation: Path, compose: bool, *, native: bool = False) -> tuple[bytes, bytes]:
    if native and not compose:
        raise ValueError("The native fixture requires lifecycle composition")
    state = json.loads((generation / "generation.json").read_bytes())
    validate_generation(state)
    if (state.get("mode") != "original-guest-bodies" or
            state.get("fixed_native_replacements") is not False or
            state.get("generation_finished") is not True or
            state.get("inputs_unchanged") is not True):
        raise ValueError("Requires a completed, unchanged original-body generation")

    source = generation / "gen" / UNIT
    original = source.read_bytes()
    updated = transform(original)
    check_stage(source, original, updated)
    start, end = function_span(updated, "f_6821C0")
    selected = updated[start:end]
    includes = b'#include "lift_prelude.h"\n' + INCLUDE
    callees = CALLEES
    appended = b""
    if compose:
        extra_include = LIFECYCLE_INCLUDE
        slot = lifecycle_call(b"SkyrimLoadSlot", b"f_67B720(c)", BOOL_RESULT)
        if native:
            extra_include += b'#include "native_load.h"\n'
            for hook in (b"enter", b"open_enter", b"open_leave"):
                old_hook = b"classic_fresh_" + hook + b"("
                new_hook = b"classic_native_load_" + hook + b"("
                selected = at_site(selected, old_hook, new_hook)
            slot = (b"    const SkyrimLifecycleCall slot = ticket ? skyrim_lifecycle_begin(SkyrimLoadSlot)"
                    b" : (SkyrimLifecycleCall){0};\n"
                    b"    classic_native_load_slot_bind(ticket, c, esi);\n"
                    b"    f_67B720(c);\n"
                    b"    if (slot.ticket) skyrim_lifecycle_end(slot, " + BOOL_RESULT + b");\n")
        selected = at_site(selected, SLOT_INVOKE, slot)
        updated = extra_include + updated[:start] + selected + updated[end:]
        includes += extra_include
        callees += VM_CALLEES
        appended = vm_fixture((generation / "gen" / VM_UNIT).read_bytes())
    prototypes = b"".join(f"void {name}(CPU *);\n".encode("ascii") for name in callees)
    return updated, includes + prototypes + selected + appended


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--ldflag", action="append", default=[])
    parser.add_argument("--sanitizers", action="store_true")
    parser.add_argument("--compose", action="store_true", help="Use the existing companion bridge with Linux memfd seals")
    args = parser.parse_args()
    if args.compose and not sys.platform.startswith("linux"):
        parser.error("--compose requires Linux memfd seals")
    generation = args.generation
    updated, fixture = lifted_fixture(generation, args.compose)
    private = (ROOT / "private").resolve()
    if private != ROOT / "private":
        raise ValueError("The hosted fixture must stay in private/")
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="classic-fresh-load-", dir=private) as scratch:
        out = Path(scratch)
        headers = out / "vendor"
        headers.mkdir()
        for filename, expected in HEADERS.items():
            data = (generation / "source" / "runtime" / filename).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError("Generated runtime header identity changed")
            (headers / filename).write_bytes(data)
        extra_sources = []
        extra_libraries = []
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
        cosave = HERE.parent / "skse-cosave"
        if args.compose:
            flags += ["-DSKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE=1", "-DSKYRIM_TEST_FRESH_COSAVE=1",
                      "-ffunction-sections", "-fdata-sections"]
            extra_sources = [HERE / "cosave_fixture.c", *(cosave / name for name in (
                "event_bridge.c", "event_trace.c", "generation.c", "immutable_ess.c", "records.c", "observer/lifecycle.c",
            ))]
            extra_libraries = ["-Wl,--gc-sections", "-lsqlite3", "-lm"]
        # Keep the transformed source and the exact function extraction private.
        (out / UNIT).write_bytes(updated)
        lifted = out / "lifted.c"
        lifted.write_bytes(fixture)
        if args.sanitizers:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        crypto = [] if sys.platform == "darwin" else ["-lcrypto"]
        binary = out / "fresh-load"
        subprocess.run([args.cc, *flags, *args.cflag, "-isystem", headers, "-I", HERE, "-I", cosave,
                        lifted, HERE / "fresh_load.c", HERE / "test_fresh_load.c", cosave / "digest.c",
                        *extra_sources, *args.ldflag, *extra_libraries, *crypto, "-o", binary], check=True, timeout=60)
        with tempfile.TemporaryDirectory(prefix="classic-fresh-load-input-") as data:
            subprocess.run([binary, data], check=True, timeout=20)


if __name__ == "__main__":
    main()
