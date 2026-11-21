#!/usr/bin/env python3
"""Stage one hosted call site in the specific reviewed private Classic lift."""
import argparse
import hashlib
import json
from pathlib import Path

GENERATION = {
    "schema": 1,
    "source_commit": "ecf814f58292d70ad5cfb19a5c61b5d8d8006a4a",
    "candidate_sha256": "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605",
    "discovery_sha256": "1fc28ce90d668d635ace8fd641d95720782e24c0f64eaf08cbaf8bf170d15b6d",
    "mode": "original-guest-bodies",
    "translated_function_count": 90390,
    "inputs_unchanged": True,
}
UNIT = Path("gen") / "exe_100.c"
UNIT_HASH = "5cac33ecc56d10529bc3a6c3df8625e05909a383e26b92a57e6f6974c2791097"
ANCHOR_MARKER = b"0x67326Du"
ANCHOR_HASH = "6b748864252dfedc686d8597659657701b150d7701f479bd97ecc633cb99bed2"
DECLARATION = b'#include "lift_voice_seam.h"\n'
INTERCEPT = b"    skyrim_fuz_host_before_voice_assign(esp, esi);\n"


def patch_unit(source):
    if hashlib.sha256(source).hexdigest() != UNIT_HASH:
        raise ValueError("Lifted source differs from the reviewed unit.")
    lines = source.splitlines(keepends=True)
    sites = [i for i, line in enumerate(lines) if ANCHOR_MARKER in line]
    if len(sites) != 1 or hashlib.sha256(lines[sites[0]].rstrip(b"\n")).hexdigest() != ANCHOR_HASH:
        raise ValueError("The original voice assignment anchor is missing or ambiguous.")
    site = sites[0]
    private_excerpt = INTERCEPT + lines[site]
    staged = DECLARATION + b"".join(lines[:site]) + INTERCEPT + b"".join(lines[site:])
    return staged, private_excerpt


def check_generation(root):
    generation = json.loads((root / "generation.json").read_text(encoding="utf-8"))
    if not isinstance(generation, dict) or any(generation.get(k) != v for k, v in GENERATION.items()):
        raise ValueError("The private generation does not match the reviewed Classic lift.")


def prepare_lift(root):
    check_generation(root)
    return patch_unit((root / UNIT).read_bytes())


def stage_lift(root, output):
    staged, _ = prepare_lift(root)
    with output.open("xb") as target:
        target.write(staged)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generation", type=Path)
    parser.add_argument("output", type=Path, help="New file in a private scratch directory.")
    args = parser.parse_args()
    stage_lift(args.generation, args.output)
