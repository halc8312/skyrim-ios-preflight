"""Extract the reviewed Classic active-effect handle and visitor bodies."""

from hashlib import sha256


UNIT = "exe_193.c"
DEPENDENCIES = ("exe_98.c", "exe_199.c", "exe_337.c", "exe_119.c")
UNIT_SHA256 = "fc151c85a6652f425fef73929585d366ab5c809486d5f1b360b9203016916218"
VISITOR_SHA256 = "2465e7efd646217c9359f32419d3a7b523898206a76070d3e4bc919575c683c7"
EFFECT_SHA256 = "f550390f7023b2375da3746cfd6b99be32bfd4fecabff2ce5396b73e2e18bf3c"
IDENTIFIER_SHA256 = "bc5a71831af1bc743116b91a79eff10f48fd7350e2ec5af8792b4fbdbd5460fa"
ACTOR_SHA256 = "58e9e3e4007fc8d29d5822b9c644a834041b03973569f99371f7394cb97780ed"
PRELUDE = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
DECLARE = (
    b"void highheels_observe_matching_effect(const CPU *, uint32_t, uint16_t);\n"
    b"void highheels_observe_identifier(const CPU *, uint32_t, uint32_t, uint32_t);\n"
)
MATCH = b"    TS32((uint32_t)(ecx + 0x8u), (uint32_t)(eax));\n"
OBSERVE = b"    highheels_observe_matching_effect(c, eax, TL16((uint32_t)(ecx + 4u)));\n"
UNLOCK = b"    ST_FENCE();\n    TS32((uint32_t)(edi), (uint32_t)(esi));\n"
SNAPSHOT = b"    highheels_observe_identifier(c, M32(esp + 0x14u), eax, edx);\n"


def function(unit, address):
    entry = f"\nvoid f_{address:X}(CPU *c) {{\n".encode()
    if unit.count(entry) != 1:
        raise ValueError(f"Expected one reviewed body at {address:#x}")
    start = unit.index(entry)
    end = unit.index(b"\n}\n", start) + len(b"\n}\n")
    return unit[start:end]


def private_fixture(unit, visitor, effect, identifier, actor):
    units = (unit, visitor, effect, identifier, actor)
    hashes = (UNIT_SHA256, VISITOR_SHA256, EFFECT_SHA256, IDENTIFIER_SHA256, ACTOR_SHA256)
    if any(sha256(body).hexdigest() != digest for body, digest in zip(units, hashes)):
        raise ValueError("Effect handle units differ from the reviewed Classic lift")
    match = function(unit, 0x8B3B00)
    if match.count(MATCH) != 1:
        raise ValueError("Expected a unique matched-effect publication")
    observed = match.replace(b"f_8B3B00(", b"observed_effect_visitor(", 1).replace(MATCH, MATCH + OBSERVE, 1)
    policy = b"".join(function(unit, address) for address in (0x8B39F0, 0x8B3B20, 0x8B3D60, 0x8B3E30, 0x8B4300))
    native = b"".join(function(effect, address) for address in (0x8D8500, 0x8D8650, 0x8D8F10))
    snapshot = function(identifier, 0xC307C0)
    if snapshot.count(UNLOCK) != 1:
        raise ValueError("Expected one reviewed identifier unlock after the handle copy")
    snapshot = snapshot.replace(UNLOCK, SNAPSHOT + UNLOCK)
    value = snapshot + function(identifier, 0xC322E0)
    return (PRELUDE + DECLARE + policy + function(visitor, 0x663410) + match + observed +
            native + value + function(actor, 0x6E9180))
