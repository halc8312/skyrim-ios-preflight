"""Run the bounded Classic typed ModEvent fixture through the pinned delivery adapter."""

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
from test_package_decision import HEADERS


SYSTEM_HASH = "61ce22b226c98264382722d0bc54d5835b32ae4821466d1f6a5b91e7af04b07c"
QUEUE_UNIT_HASH = "6ae99bf14df3c1913568b9cfb2b5842ef39c16291f73744f3781e990ef6a6025"
DISPATCH_WORD = b"(eax + 0x98u)"
DISPATCH_CALL = b"M32(esp) = 0xC4C2FBu; SPILL();"
ARGUMENT_WORD = b"(esp + 0x454u)"
COPY_CALL = b"M32(esp) = 0xC4FE05u; SPILL();"
PACKET_BUILD = b"M32(esp) = 0xC4FE3Fu; SPILL(); f_C57BB0(c);"
QUEUE_OWNERSHIP = (
    ("exe_344.c", "4bb6f489cd1d4709f1dd421af959cb4beeb0e0841d0c5e8000a87dd591651c9f",
     "f_C57BB0", (b"M32(esp) = 0xC57BE9u; SPILL(); f_A50D50(c);",
                   b"M32(esp) = 0xC57C10u; SPILL(); f_C56DB0(c);")),
    ("exe_343.c", "fcc0dde65164042e1097cdbed1656645ca6297d934fcfc87c6d2c1d7baa903cc",
     "f_C56DB0", (b"M32(esp) = 0xC56E4Au; SPILL(); f_C329E0(c);",)),
)
PROLOGUE = b'''#include "fixture.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARG(i) M32(c->r[4] + 4 + 4 * (i))
#define RET(v, n) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (n); return; } while (0)
#define GPTR(addr) (MEM + (addr))
#define LOG(...) ((void)0)
uint32_t gmalloc(unsigned bytes);
void gfree(uint32_t address);
uint32_t gstrdup(const char *text);
uint32_t rt_thunk(void (*callback)(CPU *), const char *name);
uint32_t skse_member(CPU *, uint32_t, uint32_t, int, const uint32_t *);
uint32_t skse_virtual(CPU *, uint32_t, unsigned, int, const uint32_t *);
void rt_fatal(const char *reason);
'''


def between(source: bytes, begin: bytes, end: bytes) -> bytes:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("Expected one reviewed boundary around the delivery adapter")
    start = source.index(begin)
    return source[start:source.index(end, start)]


def verify_game_queue(generation: Path) -> None:
    unit = (generation / "gen" / "exe_342.c").read_bytes()
    check_unit(unit, QUEUE_UNIT_HASH)
    start, end = function_span(unit, "f_C4C160")
    dispatch = unit[start:end]
    start, end = function_span(unit, "f_C4FCE0")
    copy = unit[start:end]
    if (dispatch.count(DISPATCH_CALL) != 1 or DISPATCH_WORD not in dispatch[:dispatch.index(DISPATCH_CALL)] or
            copy.count(COPY_CALL) != 1 or copy.count(PACKET_BUILD) != 1 or
            ARGUMENT_WORD not in copy[:copy.index(COPY_CALL)] or
            copy.index(COPY_CALL) >= copy.index(PACKET_BUILD)):
        raise ValueError("Expected the original synchronous event argument copy before packet creation")
    for file, digest, name, anchors in QUEUE_OWNERSHIP:
        unit = (generation / "gen" / file).read_bytes()
        check_unit(unit, digest)
        start, end = function_span(unit, name)
        body = unit[start:end]
        if any(body.count(anchor) != 1 for anchor in anchors):
            raise ValueError("Expected the reviewed queue name and value-copy calls in " + name)


def stage_adapter(generation: Path) -> bytes:
    runtime = generation / "source" / "runtime"
    system = (runtime / "skse_system.c").read_bytes()
    if hashlib.sha256(system).hexdigest() != SYSTEM_HASH:
        raise ValueError("Skydream delivery source differs from the reviewed input")
    state = between(system, b"typedef struct Registration {", b"static float as_float(")
    values = between(system, b"static void vm_clear(", b"static uint64_t base_handle(")
    handles = between(system, b"static void handle_ref(", b"static uint32_t resolve_form(")
    events = between(system, b"static void event_destroy(", b"static void change_registration(")
    return PROLOGUE + state + values + handles + events + (HERE / "fixture_delivery.c.in").read_bytes()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", required=True, type=Path)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((args.generation / "generation.json").read_text(encoding="utf-8"))
    validate_generation(manifest)
    if (manifest.get("mode") != "original-guest-bodies" or
            manifest.get("fixed_native_replacements") is not False or
            manifest.get("generation_finished") is not True or manifest.get("inputs_unchanged") is not True):
        raise ValueError("Requires a completed, unchanged original-body generation")
    verify_game_queue(args.generation)
    private = ROOT / "private"
    if private.resolve() != private:
        raise ValueError("Staged output must stay in private/")
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="classic-mod-events-", dir=private) as temporary:
        scratch = Path(temporary)
        for name, digest in HEADERS.items():
            header = (args.generation / "source" / "runtime" / name).read_bytes()
            if hashlib.sha256(header).hexdigest() != digest:
                raise ValueError("Generated runtime header differs from reviewed input: " + name)
            (scratch / name).write_bytes(header)
        adapter = scratch / "delivery.c"
        adapter.write_bytes(stage_adapter(args.generation))
        binary = scratch / "mod_events"
        flags = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]
        if args.sanitizers:
            flags.extend(["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"])
            if sys.platform.startswith("linux"):
                flags.extend(["-fno-pie", "-no-pie"])
        subprocess.run([args.cc, *flags, "-I", HERE, "-isystem", scratch, adapter,
                        HERE / "mod_event.c", HERE / "test_mod_events.c", "-o", binary], check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=20)


if __name__ == "__main__":
    main()
