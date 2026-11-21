# SPDX-License-Identifier: GPL-3.0-only
"""Compose a private Safety Load allocation experiment using the pinned LE lift."""
from hashlib import sha256


UNIT = "exe_259.c"
UNIT_HASH = "842247dde6c84c461377e75ba002b3b7dc1779bd33524489176820c9477b02f0"
REGION_HASH = "dd56c8b5b33a6c0e00b16f12d86a7786c8a4874bbfa6e97cffcbfbaa09093673"
DECLARATION = b"#include <stdbool.h>\nbool skyrim_safety_load_bypass_reclamation(void);\n"
START = b"L_A48E9C:\n"
END = b"L_A48EDB:\n"
ENTRY = b"\nvoid f_A48D60(CPU *c) {\n"
ANCHOR = b"    ecx = (uint32_t)((uint32_t)(esp + 0x28u));\n"

# This stage models only the small-allocation branch of the original 1.2 DLL.
# The guest retains both original fallback allocators and their cleanup.
BYPASS = (b"    if (edx == 0 && esi != 0 && esi < 0x10000000u && skyrim_safety_load_bypass_reclamation()) {\n"
          b"        M32(esp + 0x1Cu) = 0;\n"
          b"        ecx = ebp;\n"
          b"        goto L_A48EB9;\n"
          b"    }\n")


def allocator_region(unit: bytes) -> tuple[int, int]:
    if any(unit.count(marker) != 1 for marker in (ENTRY, START, END)):
        raise ValueError("Expected one reviewed Classic allocator region")
    function_start = unit.index(ENTRY)
    function_end = unit.index(b"\n}\n", function_start)
    start, end = unit.index(START), unit.index(END)
    if not function_start < start < end < function_end:
        raise ValueError("Allocator region is outside the reviewed function")
    return start, end


def stage_allocator(unit: bytes) -> bytes:
    if sha256(unit).hexdigest() != UNIT_HASH:
        raise ValueError("Allocator unit differs from the reviewed Classic lift")
    start, end = allocator_region(unit)
    region = unit[start:end]
    if sha256(region).hexdigest() != REGION_HASH or region.count(ANCHOR) != 1:
        raise ValueError("Allocator branch differs from the reviewed Classic lift")
    return DECLARATION + unit[:start] + region.replace(ANCHOR, BYPASS + ANCHOR, 1) + unit[end:]
