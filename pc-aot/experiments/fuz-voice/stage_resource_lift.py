#!/usr/bin/env python3
"""Prepare private Classic resource routines for the authored hosted runtime."""
import hashlib

from stage_lift import check_generation


RESOURCE_UNITS = {
    "exe_00.c": (
        "cd5e4755a7a1efac8839759c81b4f3b66a6bb8e34e8947dc612546af876e46e3", ("406890", "4068C0"),
    ),
    "exe_09.c": (
        "316b04f74d5c93286c8e602565e6807f2880602846dfa6a5e5642c4a7590bc2e", ("43B440",),
    ),
    "exe_257.c": (
        "c01c4fad11459983c9d080683693e158966636ba8efe3e7171561e551109db65", ("A3F810",),
    ),
    "exe_289.c": (
        "1898018ff23f7a04e24285c8c8591890c6fd4a20a1696779983ebda053a8d385", ("B08530", "B08630"),
    ),
    "exe_290.c": (
        "2341dde3a4c65cf08d5b40cb8efd880c18d9608e07283d94d71df594526546dc", ("B08970",),
    ),
}


def extract_function(source, function):
    marker = f"\nvoid f_{function}(CPU *c) {{\n".encode("ascii")
    if source.count(marker) != 1:
        raise ValueError(f"Reviewed resource routine f_{function} is missing or ambiguous.")
    start = source.index(marker)
    end = source.find(b"\n}\n", start)
    if end < 0:
        raise ValueError(f"Reviewed resource routine f_{function} has no closing boundary.")
    return source[start:end + 3]


def prepare_resources(root):
    check_generation(root)
    bodies = [b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n']
    for filename, (digest, functions) in RESOURCE_UNITS.items():
        source = (root / "gen" / filename).read_bytes()
        if hashlib.sha256(source).hexdigest() != digest:
            raise ValueError(f"{filename}: different from the reviewed resource lift.")
        bodies.extend(extract_function(source, function) for function in functions)
    return b"".join(bodies)
