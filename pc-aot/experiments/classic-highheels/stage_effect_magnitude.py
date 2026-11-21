"""Extract the reviewed Classic active-effect handle and visitor bodies."""

from hashlib import sha256


UNIT = "exe_193.c"
DEPENDENCIES = ("exe_98.c",)
UNIT_SHA256 = "fc151c85a6652f425fef73929585d366ab5c809486d5f1b360b9203016916218"
VISITOR_SHA256 = "2465e7efd646217c9359f32419d3a7b523898206a76070d3e4bc919575c683c7"
PRELUDE = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
DECLARE = b"void highheels_observe_matching_effect(const CPU *, uint32_t, uint16_t);\n"
MATCH = b"    TS32((uint32_t)(ecx + 0x8u), (uint32_t)(eax));\n"
OBSERVE = b"    highheels_observe_matching_effect(c, eax, TL16((uint32_t)(ecx + 4u)));\n"


def function(unit, address):
    entry = f"\nvoid f_{address:X}(CPU *c) {{\n".encode()
    if unit.count(entry) != 1:
        raise ValueError(f"Expected one reviewed body at {address:#x}")
    start = unit.index(entry)
    end = unit.index(b"\n}\n", start) + len(b"\n}\n")
    return unit[start:end]


def private_fixture(unit, visitor):
    if sha256(unit).hexdigest() != UNIT_SHA256 or sha256(visitor).hexdigest() != VISITOR_SHA256:
        raise ValueError("Effect handle units differ from the reviewed Classic lift")
    match = function(unit, 0x8B3B00)
    if match.count(MATCH) != 1:
        raise ValueError("Expected a unique matched-effect publication")
    observed = match.replace(b"f_8B3B00(", b"observed_effect_visitor(", 1).replace(MATCH, MATCH + OBSERVE, 1)
    policy = b"".join(function(unit, address) for address in (0x8B39F0, 0x8B3B20, 0x8B3E30))
    return PRELUDE + DECLARE + policy + function(visitor, 0x663410) + match + observed
