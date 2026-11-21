"""Extract an owner-only fixture for the reviewed Classic item's VM delivery."""

from hashlib import sha256


UNIT = "exe_197.c"
UNIT_SHA256 = "6e1f2aaa33f9b75f741a1bbca00af5932a4864f75df601aa13e2586a2762beea"
ENTRY = b"\nvoid f_8C9910(CPU *c) {\n"
CALL_RETURN = b"M32(esp) = 0x8C9B6Au;"
NEXT = b" RELOAD(); }\n"
PRELUDE = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
DECLARE = b"void highheels_observe_item_delivery(const CPU *cpu, uint8_t phase);\n"
CAPTURE = b"    const uint8_t item_phase = TL8((uint32_t)(esi + 0xEu));\n"
OBSERVE = b"    highheels_observe_item_delivery(c, item_phase);\n"


def private_fixture(unit):
    if sha256(unit).hexdigest() != UNIT_SHA256:
        raise ValueError("Item event unit differs from the reviewed Classic lift")
    if unit.count(ENTRY) != 1:
        raise ValueError("Expected one reviewed item event dispatcher")
    start = unit.index(ENTRY)
    end = unit.index(b"\n}\n", start) + len(b"\n}\n")
    body = unit[start:end]
    if body.count(CALL_RETURN) != 1:
        raise ValueError("Expected one reviewed item delivery return")
    call = body.index(CALL_RETURN)
    begin_call = body.rindex(b"\n", 0, call) + 1
    resume = body.index(NEXT, call) + len(NEXT)
    baseline = body.replace(b"f_8C9910(", b"unobserved_item_delivery(", 1)
    return PRELUDE + DECLARE + baseline + body[:begin_call] + CAPTURE + body[begin_call:resume] + OBSERVE + body[resume:]
