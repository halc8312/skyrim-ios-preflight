# SPDX-License-Identifier: GPL-3.0-only
"""Instrument the shared original IMenu::AdvanceMovie in the private hosted trial."""
from hashlib import sha256

UNIT = "exe_263.c"
UNIT_SHA = "a3e29ba9157cd22ff14008090e5778b3b655f69e85bbdaa5ef2abfcef3de15d3"
BODY_SHA = "6ad79487be0d662f49e5d2137668d83120581841e4d2baeba1a289cd95b0c1bf"
ENTRY = b"void f_A64980(CPU *c) {\n"
ANCHOR = b"\nL_A6498D:\n"
CALL = (b"    if (skyrim_menu_focus_callback && (M32(esi) == 0x10e5b90u || M32(esi) == 0x10e6594u)) {\n"
        b"        SPILL(); skyrim_menu_focus_callback(c, esi, ecx); RELOAD();\n"
        b"    }\n")
DECL = b"struct CPU;\nextern void (*skyrim_menu_focus_callback)(struct CPU *, unsigned, unsigned);\n"


def staged_advance(unit: bytes) -> bytes:
    if sha256(unit).hexdigest() != UNIT_SHA or unit.count(ENTRY) != 1:
        raise ValueError("Menu advance unit differs from the reviewed Classic lift")
    start = unit.index(ENTRY)
    body = unit[start:unit.index(b"\n}\n", start) + 3]
    if sha256(body).hexdigest() != BODY_SHA or body.count(ANCHOR) != 1:
        raise ValueError("Menu advance body differs from the reviewed Classic lift")
    return body.replace(ANCHOR, ANCHOR + CALL, 1)


def stage(unit: bytes) -> bytes:
    changed = staged_advance(unit)
    original = changed.replace(CALL, b"", 1)
    return DECL + unit.replace(original, changed, 1)
