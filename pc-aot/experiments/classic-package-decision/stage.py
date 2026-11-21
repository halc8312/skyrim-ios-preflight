"""Stage a reviewed lifted call site for a local hosted fixture."""

from dataclasses import dataclass
import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from classic_skse_guest import validate_generation


PRELUDE = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'


@dataclass(frozen=True)
class Site:
    fixture: str
    unit: str
    sha256: str
    function: str
    anchor: bytes
    updated: bytes

    @property
    def definition(self) -> bytes:
        return function_definition(self.function)

    @property
    def include(self) -> bytes:
        return f'#include "{self.fixture}.h"\n'.encode("ascii")


DECISION_CALL = b"    esp -= 4; M32(esp) = 0x6A9E95u; SPILL(); f_40E4F0(c); RELOAD();\n"
PACKAGE_DECISION = Site(
    "package_decision", "exe_109.c",
    "0c52f6d597fe307a6fa47f02b02ba0e201e48333f16c4a9dd7064de2e829adf5", "f_6A9E60",
    DECISION_CALL,
    DECISION_CALL + b"    eax = classic_package_decision_after_call(c, esi, eax);\n",
)

CHANGE_CALL = b"    esp -= 4; M32(esp) = 0x8CAB59u; SPILL(); f_A51280(c); RELOAD();\n"
PACKAGE_CHANGE = Site(
    "package_change", "exe_197.c",
    "6e1f2aaa33f9b75f741a1bbca00af5932a4864f75df601aa13e2586a2762beea", "f_8CA9B0",
    CHANGE_CALL,
    b"    esp -= 4; M32(esp) = 0x8CAB59u; SPILL();\n"
    b"    classic_package_change_before_assign(c, edi);\n"
    b"    f_A51280(c); RELOAD();\n",
)

IDLE_LABEL = b"L_6F02FF:\n"
IDLE_TEST = b"    (void)logic8((uint8_t)(TL8((uint32_t)(esi + 0x2Cu)) & (uint8_t)0x1u), &f);\n"
IDLE_EVENT = Site(
    "idle_event", "exe_121.c",
    "d8f454a6da3955b125c270f72ca4835f54a4010da36f964e579c33224b04c5a3", "f_6F02D0",
    IDLE_LABEL + IDLE_TEST,
    IDLE_LABEL + b"    SPILL(); classic_idle_event_before_test(c, esi);\n" + IDLE_TEST,
)


def function_definition(name: str) -> bytes:
    return ("\nvoid " + name + "(CPU *c) {\n").encode("ascii")


def function_span(unit: bytes, name: str) -> tuple[int, int]:
    definition = function_definition(name)
    if unit.count(definition) != 1:
        raise ValueError("Expected one original definition of " + name)
    start = unit.index(definition)
    end = unit.index(b"\n}\n", start) + len(b"\n}\n")
    return start, end


def transform(unit: bytes, site: Site) -> bytes:
    start, end = function_span(unit, site.function)
    if unit.count(site.anchor) != 1 or site.anchor not in unit[start:end]:
        raise ValueError("Expected one original instruction site inside " + site.function)
    check_unit(unit, site.sha256)
    unit = unit.replace(site.anchor, site.updated, 1)
    return PRELUDE + site.include + unit[len(PRELUDE):]


def check_unit(unit: bytes, digest: str) -> None:
    if not unit.startswith(PRELUDE) or hashlib.sha256(unit).hexdigest() != digest:
        raise ValueError("Generated unit differs from the reviewed input")


def stage(generation: Path, output: Path, site: Site) -> Path:
    source = generation / "gen" / site.unit
    output = output.resolve()
    if not output.is_relative_to(ROOT / "private"):
        raise ValueError("The staged game-derived source must stay in private/")
    state = json.loads((generation / "generation.json").read_text(encoding="utf-8"))
    validate_generation(state)
    if (state.get("mode") != "original-guest-bodies" or
            state.get("fixed_native_replacements") is not False or
            state.get("generation_finished") is not True or
            state.get("inputs_unchanged") is not True):
        raise ValueError("Requires a completed, unchanged original-body generation")

    updated = transform(source.read_bytes(), site)
    target = output / site.unit
    with target.open("xb") as f:
        f.write(updated)
    return target
