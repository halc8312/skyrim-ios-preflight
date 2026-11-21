# SPDX-License-Identifier: GPL-3.0-only
"""Stage an observer at two reviewed Classic menu dispatch calls."""
from hashlib import sha256
import re

UNIT = "exe_262.c"
UNIT_SHA256 = "d9a1ccfb8825564cd5d3be6e41cf70d96fcf24678cb16b9b1c096688517a959c"
ANCHORS = {
    0xA5D6BC: "13aea3fac5b639b986b6791d25eb3ffef0936f31cee326bddf883e737d08f15d",
    0xA5D9CF: "88c259ad0273072159dc8c1f1e829505cb52bd24cc2baa35e5613c50bf3f8156",
}
DECLARATION = (b"#include <stdint.h>\n"
               b"void skyrim_loading_menu_observe(uint32_t site, uint32_t dispatcher, uint32_t event);\n")
ENTRY = b"\nvoid f_A5D370(CPU *c) {\n"
CALL = b"f_6EBC10(c);"


def dispatch_lines(unit: bytes, staged: bool = False) -> dict[int, bytes]:
    if not staged and sha256(unit).hexdigest() != UNIT_SHA256:
        raise ValueError("Menu unit differs from the reviewed Classic lift")
    if unit.count(ENTRY) != 1:
        raise ValueError("Expected one Classic menu update function")
    start = unit.index(ENTRY) + len(ENTRY)
    end = unit.index(b"\n}\n", start)
    lines = {}
    for site, digest in ANCHORS.items():
        prefix = f"    esp -= 4; M32(esp) = 0x{site + 5:X}u; SPILL(); ".encode("ascii")
        lines_at_site = re.findall(rb"(?m)^" + re.escape(prefix) + rb"[^\n]*\n", unit[start:end])
        if len(lines_at_site) != 1:
            raise ValueError("Classic menu dispatch anchor is missing or ambiguous")
        line = lines_at_site[0]
        original = line.removeprefix(prefix)
        if staged:
            original = original.removeprefix(observer_call(site))
        if sha256(prefix + original).hexdigest() != digest:
            raise ValueError("Classic menu dispatch anchor differs from the reviewed line")
        lines[site] = line
    return lines


def observer_call(site: int) -> bytes:
    return f"skyrim_loading_menu_observe(0x{site:X}u, c->r[1], M32(c->r[4] + 4u)); ".encode("ascii")


def stage_loading_menu(unit: bytes) -> bytes:
    for site, line in dispatch_lines(unit).items():
        if unit.count(line) != 1:
            raise ValueError("Classic menu dispatch anchor occurs outside the reviewed call")
        unit = unit.replace(line, line.replace(CALL, observer_call(site) + CALL, 1), 1)
    return DECLARATION + unit
