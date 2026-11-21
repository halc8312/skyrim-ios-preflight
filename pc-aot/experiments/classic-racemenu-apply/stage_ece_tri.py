"""Trace the fixed Classic FaceGen TRI path immediately around its stock reader."""
from hashlib import sha256

UNIT = "exe_66.c"
DIGEST = "e5aef726544cfa9c4bacf1661bae164b8cd86b6c7e36d2190b201ffd8a8ccd5a"
DECLARATION = (b"#include <stdint.h>\n"
               b"uint64_t skyrim_ece_tri_begin(const CPU *, uint32_t path, uint32_t stream);\n"
               b"void skyrim_ece_tri_finish(const CPU *, uint64_t ticket, uint8_t parsed);\n")
RESOLVE = b"    esp -= 4; M32(esp) = 0x59F2A1u; SPILL(); f_A3F810(c); RELOAD();\n"
PARSE = b"    esp -= 4; M32(esp) = 0x59F2B5u; SPILL(); f_59EF90(c); RELOAD();\n"
CAPTURE = b"    const uint32_t skyrim_ece_tri_path = eax;\n"
BEGIN = (b"    const uint64_t skyrim_ece_tri_ticket = "
         b"skyrim_ece_tri_begin(c, skyrim_ece_tri_path, M32(esp + 0x14u));\n")
FINISH = b"    if (skyrim_ece_tri_ticket) skyrim_ece_tri_finish(c, skyrim_ece_tri_ticket, (uint8_t)eax);\n"


def stage(unit: bytes) -> bytes:
    if sha256(unit).hexdigest() != DIGEST:
        raise ValueError("FaceGen TRI unit differs from the reviewed Classic lift")
    header = b'\nvoid f_59F270(CPU *c) {\n'
    if unit.count(header) != 1:
        raise ValueError("Expected one Classic FaceGen TRI entry")
    start = unit.index(header)
    end = unit.index(b"\n}\n", start)
    body = unit[start:end]
    if body.count(RESOLVE) != 1 or body.count(PARSE) != 1:
        raise ValueError("Expected one resolver and parser in the Classic FaceGen TRI entry")
    staged = body.replace(RESOLVE, CAPTURE + RESOLVE).replace(PARSE, BEGIN + PARSE + FINISH)
    includes = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
    if not unit.startswith(includes):
        raise ValueError("Classic FaceGen TRI includes differ")
    return unit[:len(includes)] + DECLARATION + unit[len(includes):start] + staged + unit[end:]
