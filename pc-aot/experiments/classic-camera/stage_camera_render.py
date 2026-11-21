"""Observe the retained-list handoff in the reviewed Classic render path."""
from hashlib import sha256

RENDER_UNIT = "exe_107.c"
RENDER_SHA256 = "c0a140d6fe5a3c86e3f0c970e669c8911f787ce14a72ff7da352e1c0c82e85d7"
ENTRY = b"void f_69A9F0(CPU *c) {\n"
DECLARATION = b"#include <stdint.h>\nvoid skyrim_camera_observe_render(uint32_t frame, uint32_t first, uint32_t second);\n"
CAPTURE = b"    const uint32_t skyrim_camera_frame_return = M32(c->r[4]);\n"
CALL = b"    esp -= 4; M32(esp) = 0x69AB11u; SPILL(); f_CB9E20(c); RELOAD();\n"
OBSERVE = (b"    if (skyrim_camera_frame_return == 0x69B96Fu)\n"
           b"        skyrim_camera_observe_render(edi, eax, edx);\n")


def render_function(unit: bytes) -> bytes:
    marker = b"\n" + ENTRY
    if unit.count(marker) != 1:
        raise ValueError("Expected one original Classic render function")
    start = unit.index(marker) + 1
    end = unit.find(b"\n}\n", start)
    if end < 0:
        raise ValueError("The reviewed Classic render function is incomplete")
    return unit[start:end + 3]


def stage_camera_render(unit: bytes) -> bytes:
    if sha256(unit).hexdigest() != RENDER_SHA256:
        raise ValueError("Camera render unit differs from the reviewed Classic lift")
    body = render_function(unit)
    if body.count(CALL) != 1:
        raise ValueError("The reviewed retained-list call is missing or ambiguous")
    changed = body.replace(ENTRY, ENTRY + CAPTURE, 1).replace(CALL, OBSERVE + CALL, 1)
    return DECLARATION + unit.replace(body, changed, 1)
