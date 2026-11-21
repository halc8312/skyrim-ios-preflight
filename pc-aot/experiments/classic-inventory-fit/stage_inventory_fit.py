# SPDX-License-Identifier: GPL-3.0-only
"""Private A/B preparation for one Classic inventory movie scale mode."""
from hashlib import sha256


UNIT = "exe_262.c"
INVENTORY_UNIT = "exe_181.c"
UNIT_HASH = "d9a1ccfb8825564cd5d3be6e41cf70d96fcf24678cb16b9b1c096688517a959c"
CALLER_HASH = "2da411cb585d09630df9631aeb13f69cf03ccba85afc0ba2254587e8f87fc259"
INVENTORY_CALL_HASH = "3255035def804449a2e2548c64477b7642a4bf5c804305427cb38f4b0a7dfe0b"
MODE_CALL_HASH = "0455f25389096492450547a5de6af5aaa4c5a11b234538cb30945ca8350317a1"
DECLARATION = b"#include <stdbool.h>\nbool skyrim_inventory_fit_enabled(void);\n"
LOAD_ENTRY = b"\nvoid f_A60C80(CPU *c) {\n"
INVENTORY_ENTRY = b"\nvoid f_869A70(CPU *c) {\n"
START = b"L_A60D2F:\n"
ANCHOR = b"    edx = (uint32_t)(TL32((uint32_t)(edx + 0x6Cu)));\n"
FIT = (b"    if (eax == 3u && M32(esp + 0x178u) == 0x10E5B7Cu && skyrim_inventory_fit_enabled()) {\n"
       b"        const uint32_t width = TL32(0x12CF5B0u), height = TL32(0x12CF5B4u);\n"
       b"        if (width && height && width <= 16384u && height <= 16384u && width * 9u > height * 16u + 16u)\n"
       b"            eax = 1u;\n"
       b"    }\n")


def function_body(unit: bytes, entry: bytes) -> bytes:
    if unit.count(entry) != 1:
        raise ValueError("Expected one reviewed menu entry")
    start = unit.index(entry) + len(entry)
    return unit[start:unit.index(b"\n}\n", start)]


def mode_call(unit: bytes) -> bytes:
    body = function_body(unit, LOAD_ENTRY)
    start = body.index(START)
    end = body.index(b"\n", body.index(b"0xA60D45u;", start)) + 1
    return body[start:end]


def stage_inventory_fit(unit: bytes, inventory: bytes) -> bytes:
    if sha256(unit).hexdigest() != UNIT_HASH or sha256(inventory).hexdigest() != CALLER_HASH:
        raise ValueError("Menu units differ from the reviewed Classic lift")
    caller = function_body(inventory, INVENTORY_ENTRY)
    start = caller.index(b"{ uint32_t v_ = 0x3u;", caller.index(b"0x869AA9u;"))
    end = caller.index(b"\n", caller.index(b"0x869AD7u;", start)) + 1
    if sha256(caller[start:end]).hexdigest() != INVENTORY_CALL_HASH:
        raise ValueError("Inventory movie load differs from the reviewed Classic lift")
    region = mode_call(unit)
    if sha256(region).hexdigest() != MODE_CALL_HASH or unit.count(region) != 1 or region.count(ANCHOR) != 1:
        raise ValueError("Menu mode call differs from the reviewed Classic lift")
    return DECLARATION + unit.replace(region, region.replace(ANCHOR, FIT + ANCHOR, 1), 1)
