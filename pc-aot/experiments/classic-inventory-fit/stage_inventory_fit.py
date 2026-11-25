# SPDX-License-Identifier: GPL-3.0-only
"""Private preparation for reviewed Classic menu movie scale modes."""
from hashlib import sha256


UNIT = "exe_262.c"
UNIT_HASH = "d9a1ccfb8825564cd5d3be6e41cf70d96fcf24678cb16b9b1c096688517a959c"
MODE_CALL_HASH = "0455f25389096492450547a5de6af5aaa4c5a11b234538cb30945ca8350317a1"
DECLARATION = b"#include <stdbool.h>\nbool skyrim_inventory_fit_enabled(void);\n"
LOAD_ENTRY = b"\nvoid f_A60C80(CPU *c) {\n"
START = b"L_A60D2F:\n"
ANCHOR = b"    edx = (uint32_t)(TL32((uint32_t)(edx + 0x6Cu)));\n"
# Unit, constructor, return address, movie name pointer, and call slice SHA-256.
# The names and modes were checked from the fixed LE 1.9.32 input. These menus
# have content at their edges. Full-screen effects and 3D overlays keep stock.
MENU_CALLERS = (
    ("exe_175.c", "842690", "842707", "10E3850", "24263b0f09fb24f28c9560b18d2c42b015d900909379a0eb09c2142a077a4c66"),  # Barter
    ("exe_176.c", "845550", "8455DD", "10E3A88", "d31bd87db2b733da77e9ca46c612bf51ad21fb04046b14aac37df0b9b5a2735a"),  # BookMenu
    ("exe_176.c", "8490C0", "849148", "10E4084", "05e15a32c621be5f3f8538029dfba4bfe278c1d50e1c3a2ae4c1e4de4957b639"),  # Container
    ("exe_177.c", "84EAF0", "84EB2B", "10E4348", "aa69bc5737c358c97c9fc85ae11ee60d8cbaa1425719f60a25491fdb625b03e6"),  # Crafting
    ("exe_179.c", "85A1D0", "85A227", "10E4C7C", "ae372b2ba606b45ab9dd3faa96c51ad349953596cc55af0c3776551424ca31a8"),  # Dialogue
    ("exe_179.c", "85CEC0", "85CF21", "10E504C", "fea9e5b498165ff426e9fc75d0dc74c272e366b92fb1957c5083bc71dae30436"),  # Gift
    ("exe_181.c", "869A70", "869AD7", "10E5B7C", "3255035def804449a2e2548c64477b7642a4bf5c804305427cb38f4b0a7dfe0b"),  # Inventory
    ("exe_183.c", "873760", "8737AC", "10E6584", "b6b7761a8f1c507444d24b254acae3bcd7ce7d2c3616433d53e791c2189d60dc"),  # Magic
    ("exe_186.c", "886930", "886979", "10E7574", "4c31043f0cf12f895fec1518001fc6d51628185c9bae6ad09c115ceeeab98d3e"),  # RaceSex
    ("exe_188.c", "893C70", "893CE0", "10E7FE4", "36af4d51c579a8040f20f2a84dd7268e9a6163d87e5f5027ad19732c94b40fbd"),  # Training
)
CALLER_UNITS = tuple(sorted({site[0] for site in MENU_CALLERS}))
FIT = (b"    if (eax == 3u && skyrim_inventory_fit_enabled()) {\n"
       b"        switch (M32(esp + 0x178u)) {\n"
       + b"".join(f"        case 0x{site[3]}u:\n".encode("ascii") for site in MENU_CALLERS)
       + b"        {\n"
       b"            const uint32_t width = TL32(0x12CF5B0u), height = TL32(0x12CF5B4u);\n"
       b"            if (width && height && width <= 16384u && height <= 16384u && width * 9u > height * 16u + 16u)\n"
       b"                eax = 1u;\n"
       b"            break;\n"
       b"        }\n"
       b"        }\n"
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


def stage_inventory_fit(unit: bytes, callers: dict[str, bytes]) -> bytes:
    if sha256(unit).hexdigest() != UNIT_HASH or set(callers) != set(CALLER_UNITS):
        raise ValueError("Menu units differ from the reviewed Classic lift")
    for source, entry, returned, name, digest in MENU_CALLERS:
        caller = function_body(callers[source], f"\nvoid f_{entry}(CPU *c) {{\n".encode("ascii"))
        end = caller.index(f"M32(esp) = 0x{returned}u; SPILL(); f_A60C80(c);".encode("ascii"))
        name_push = caller.rindex(f"{{ uint32_t v_ = 0x{name}u;".encode("ascii"), 0, end)
        mode_push = caller.rindex(b"{ uint32_t v_ = 0x3u;", 0, name_push)
        if sha256(caller[mode_push:caller.index(b"\n", end) + 1]).hexdigest() != digest:
            raise ValueError(f"Movie caller {entry} differs from the reviewed Classic lift")
    region = mode_call(unit)
    if sha256(region).hexdigest() != MODE_CALL_HASH or unit.count(region) != 1 or region.count(ANCHOR) != 1:
        raise ValueError("Menu mode call differs from the reviewed Classic lift")
    return DECLARATION + unit.replace(region, region.replace(ANCHOR, FIT + ANCHOR, 1), 1)


def combine_with_observation(observed: bytes, fitted: bytes) -> bytes:
    # Each stage has already checked the original unit independently. The
    # observer's edits are in f_A5D370; this only replaces f_A60C80's call.
    before, after = mode_call(observed), mode_call(fitted)
    if (sha256(before).hexdigest() != MODE_CALL_HASH or observed.count(before) != 1
            or after.count(FIT) != 1 or after.replace(FIT, b"", 1) != before):
        raise ValueError("Menu scale mode call overlaps the menu observation")
    return DECLARATION + observed.replace(before, after, 1)
