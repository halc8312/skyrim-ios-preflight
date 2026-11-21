"""Exercise one copied actor field pair through pinned Classic lookup code."""

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
PACKAGE = HERE.parent / "classic-package-decision"
COSAVE = HERE.parent / "skse-cosave"
sys.path.insert(0, str(PACKAGE))
from stage import check_unit, function_span, validate_generation
from test_package_decision import HEADERS


UNITS = (
    ("exe_203.c", "d82de496f7f9b698300a10c95beeecad1b9d67e75eb28beac11f2f5cafd31d4d",
     ("f_8F0C10",)),
    ("exe_12.c", "c3043212335f627e2674517e7610619355eef51fd376297c4e2563413f1dbb34",
     ("f_451A30",)),
    ("exe_259.c", "842247dde6c84c461377e75ba002b3b7dc1779bd33524489176820c9477b02f0",
     ("f_A4A570", "f_A4AEE0", "f_A4B0F0")),
)
MANAGED_UNITS = (
    ("exe_341.c", "e5c36dcbc65835ea7f06ac512f407d1177e0d3cb1766fc1b8fceaf3cf262d2e0",
     ("f_C46CB0",)),
    ("exe_201.c", "1d591988957cef7fcbb8a194f415692623703b5b83ccc0d20205726c22979df5",
     ("f_8E5B90", "f_8E1D30")),
    ("exe_199.c", "f550390f7023b2375da3746cfd6b99be32bfd4fecabff2ce5396b73e2e18bf3c",
     ("f_8D9510", "f_8D94A0")),
    ("exe_337.c", "bc5a71831af1bc743116b91a79eff10f48fd7350e2ec5af8792b4fbdbd5460fa",
     ("f_C307C0", "f_C322E0")),
    ("exe_339.c", "d62bb38e7b58b6db2f9999cd6dcac6fa5cbf524f0375b250a20e101b1e7bca94",
     ("f_C3A620", "f_C3AC40", "f_C3A830")),
)
MANAGED_SERVICES = (
    "f_C329E0", "f_C328E0", "f_C4D650", "f_505D00", "f_4202A0", "f_C395D0",
    "f_C398A0", "f_8C11D0", "f_F520BC",
)
RETAINED_UNITS = (
    ("exe_132.c", "ad21bbdf90ee77aa0747db86732ae10210c736778013e4966e56f4ac15b7b0df",
     ("f_739FD0",)),
    ("exe_27.c", "aff5a05e804852f9be100bcaa715b6e43b1bb6c526b69de7671548c6c49380d4",
     ("f_4A9180",)),
    ("exe_26.c", "065423b645414d998af80bfe7f3be8080800f0d2a0012b1d8971e5f10e22a596",
     ("f_4A81B0",)),
    ("exe_09.c", "316b04f74d5c93286c8e602565e6807f2880602846dfa6a5e5642c4a7590bc2e",
     ("f_43C400",)),
    ("exe_259.c", "842247dde6c84c461377e75ba002b3b7dc1779bd33524489176820c9477b02f0",
     ("f_A4AF50",)),
)
RETAINED_SERVICES = (
    "f_402B20", "f_402BE0", "f_402C00", "f_40A760", "f_431B00", "f_4DE820",
    "f_515DE0", "f_54B400", "f_771200", "f_85FBD0", "f_A5C300",
)
HIT = (b"L_451A93:\n"
       b"    esi = (uint32_t)(TL32((uint32_t)(eax + 0x4u)));\n")
OBSERVE = b"    classic_test_copy_actor(c, M32((uint32_t)(esp + 0xCu)), edi, esi);\n"
GET_FORM_CALL = b"    esp -= 4; M32(esp) = 0x8F0C1Au; SPILL(); f_451A30(c); RELOAD();\n"
MANAGED_UNPACK = b"    esp -= 4; M32(esp) = 0x8E5BBDu; SPILL(); f_8D9510(c); RELOAD();\n"
MANAGED_OBSERVE = b"    classic_test_managed_actor(c, ebx, esi, edi, M32(esp), eax);\n"
RETAINED_CALL = b"    esp -= 4; M32(esp) = 0x73A039u; SPILL(); f_4A9180(c); RELOAD();\n"
RETAINED_RELEASE = b"M32(esp) = 0x73A234u; SPILL();"
RETAINED_OBSERVE = b"    classic_test_retained_actor(c, (uint8_t)eax, M32(esp), M32(esp + 4));\n"
ROUNDTRIP_NAMES = (
    b"#define MEM classic_form_projection_memory\n"
    b"#define rt_call_ic classic_form_projection_call_ic\n"
)
MANAGED_ROUNDTRIP_NAMES = (
    b"#define MEM classic_managed_actor_memory\n"
    b"#define rt_call_ic classic_managed_actor_call_ic\n"
    b"#define f_C398A0 classic_managed_actor_log_unreachable\n"
)


def stage_lift(generation: Path, roundtrip: bool = False, managed: bool = False,
               retained: bool = False) -> bytes:
    state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
    validate_generation(state)
    if (state.get("mode") != "original-guest-bodies" or
            state.get("fixed_native_replacements") is not False or
            state.get("generation_finished") is not True or
            state.get("inputs_unchanged") is not True):
        raise ValueError("Requires a completed, unchanged original-body generation")

    pieces = [b'#include "lift_prelude.h"\n']
    if roundtrip:
        pieces.insert(0, MANAGED_ROUNDTRIP_NAMES if managed else ROUNDTRIP_NAMES)
    if retained:
        units, services = RETAINED_UNITS, RETAINED_SERVICES
        pieces.append(b"void classic_test_retained_actor(const CPU *, uint8_t, uint32_t, uint32_t);\n")
    elif managed:
        units, services = MANAGED_UNITS, MANAGED_SERVICES
        pieces.append(b"void classic_test_managed_actor(const CPU *, uint32_t, uint32_t, "
                      b"uint32_t, uint32_t, uint32_t);\n")
    else:
        units, services = UNITS, ()
        pieces.append(b"void classic_test_copy_actor(const CPU *, uint32_t, uint32_t, uint32_t);\n")
    pieces.extend(f"void {name}(CPU *);\n".encode("ascii") for name in services)
    pieces.extend(f"void {name}(CPU *);\n".encode("ascii")
                  for _, _, names in units for name in names)
    for filename, digest, names in units:
        source = (generation / "gen" / filename).read_bytes()
        check_unit(source, digest)
        for name in names:
            start, end = function_span(source, name)
            body = source[start:end]
            if name == "f_8F0C10" and body.count(GET_FORM_CALL) != 1:
                raise ValueError("Expected one original Game.GetForm lookup")
            if name == "f_451A30":
                if body.count(HIT) != 1:
                    raise ValueError("Expected one original lookup hit before unlock")
                body = body.replace(HIT, HIT + OBSERVE, 1)
            if name == "f_8E5B90":
                if body.count(MANAGED_UNPACK) != 1:
                    raise ValueError("Expected one Actor argument unpack before its callback")
                body = body.replace(MANAGED_UNPACK, MANAGED_UNPACK + MANAGED_OBSERVE, 1)
            if name == "f_739FD0":
                if (body.count(RETAINED_CALL) != 1 or body.count(RETAINED_RELEASE) != 1 or
                        body.index(RETAINED_CALL) >= body.index(RETAINED_RELEASE)):
                    raise ValueError("Expected one scoped Crosshair lookup before its release")
                body = body.replace(RETAINED_CALL, RETAINED_CALL + RETAINED_OBSERVE, 1)
            pieces.append(body)
    return b"".join(pieces)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitizers", action="store_true")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--managed-actor", action="store_true")
    mode.add_argument("--retained-actor", action="store_true")
    args = parser.parse_args()
    private = (ROOT / "private").resolve()
    if private != ROOT / "private":
        raise ValueError("Staged sources must stay in private/")
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="classic-form-projection-", dir=private) as scratch:
        out = Path(scratch)
        headers = out / "vendor"
        headers.mkdir()
        for name, digest in HEADERS.items():
            header = (args.generation / "source" / "runtime" / name).read_bytes()
            if hashlib.sha256(header).hexdigest() != digest:
                raise ValueError("Generated runtime header differs from reviewed input: " + name)
            (headers / name).write_bytes(header)
        lifted = out / "lifted.c"
        lifted.write_bytes(stage_lift(args.generation, managed=args.managed_actor,
                                      retained=args.retained_actor))
        fixture = "test_form_projection"
        if args.managed_actor:
            fixture = "test_managed_actor"
        if args.retained_actor:
            fixture = "test_retained_actor"
        binary = out / fixture
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g"]
        if args.sanitizers:
            flags.extend(["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                          "-fno-omit-frame-pointer"])
        trace = []
        if fixture == "test_form_projection":
            trace = [COSAVE / "event_trace.c", COSAVE / "observer" / "lifecycle.c"]
        subprocess.run([args.cc, *flags, "-isystem", headers, "-I", PACKAGE, "-I", COSAVE,
                        lifted, HERE / (fixture + ".c"), PACKAGE / "form_view.c",
                        *trace, "-o", binary], check=True, timeout=60)
        subprocess.run([binary], check=True, timeout=20)


if __name__ == "__main__":
    main()
