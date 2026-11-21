"""Prepare the reviewed Classic camera transition's observer call."""
from hashlib import sha256

CAMERA_UNIT = "exe_95.c"
CAMERA_SHA256 = "96277ab1a6366dd68725d4ebced580c9f29f4f6a52be32f9034f14b3d2896f0b"
ENTRY = b"void f_6533D0(CPU *c) {\n"
EXIT = b"SPILL(); return;"
DECLARATION = b"#include <stdint.h>\nvoid skyrim_camera_observe_return(uint32_t receiver);\n"
CAPTURE = b"    const uint32_t skyrim_camera_receiver = c->r[1];\n"
OBSERVED_EXIT = b"SPILL(); skyrim_camera_observe_return(skyrim_camera_receiver); return;"


def camera_function(unit: bytes) -> bytes:
    marker = b"\n" + ENTRY
    if unit.count(marker) != 1:
        raise ValueError("Expected one original Classic camera transition")
    start = unit.index(marker) + 1
    end = unit.find(b"\n}\n", start)
    if end < 0:
        raise ValueError("The reviewed Classic camera transition is incomplete")
    return unit[start:end + 3]


def stage_camera(unit: bytes) -> bytes:
    if sha256(unit).hexdigest() != CAMERA_SHA256:
        raise ValueError("Camera unit differs from the reviewed Classic lift")
    body = camera_function(unit)
    if body.count(b"return;") != 1 or body.count(EXIT) != 1:
        raise ValueError("The reviewed camera transition exit is missing or ambiguous")
    changed = body.replace(ENTRY, ENTRY + CAPTURE, 1).replace(EXIT, OBSERVED_EXIT, 1)
    return DECLARATION + unit.replace(body, changed, 1)
