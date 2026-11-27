#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Add lightweight mode's three calls to the staged runtime's ini functions.

Run by scripts/build-title-host.sh on the staged skyrim_shims.c, after prepare_runtime.py. It is a separate
step on purpose: prepare_runtime.py is an input of the pipeline-cache identity, and lightweight mode changes
nothing the renderer sees, so the identity (and the shader caches already on a device) must stay as they were.

With the mode off every added call returns NULL / 0, so the ini reads and writes behave exactly as before.
The hunks are data so a test can state precisely what this adds. Every anchor must match exactly once.
"""
import argparse
from pathlib import Path
import sys

INCLUDE = ('#include "mobile_ui.h"', '#include "mobile_ui.h"\n#include "lightweight_core.h"')
HUNKS = (
    ('        const char *v = f ? ini_find(f, app, key) : NULL;\n        if (!v) v = def ? def : "";',
     '        const char *v = f ? ini_find(f, app, key) : NULL;\n'
     '        char light[48];\n'
     '        const char *lv = skyrim_light_read(GSTR(ARG(5)), app, key, v, def, light, sizeof light);\n'
     '        if (lv) v = lv;\n'
     '        if (!v) v = def ? def : "";',
     "lightweight string read"),
    ('    uint32_t r = v ? (uint32_t)strtol(v, NULL, 10) : ARG(2);',
     '    char light[48], fallback[16];\n'
     '    snprintf(fallback, sizeof fallback, "%d", (int)(int32_t)ARG(2));\n'
     '    const char *lv = ARG(0) && ARG(1) ?\n'
     '        skyrim_light_read(GSTR(ARG(3)), GSTR(ARG(0)), GSTR(ARG(1)), v, fallback, light, sizeof light) : NULL;\n'
     '    if (lv) v = lv;\n'
     '    uint32_t r = v ? (uint32_t)strtol(v, NULL, 10) : ARG(2);',
     "lightweight integer read"),
    ('    if (!app) RET(1, 4);\n    pthread_mutex_lock(&g_ini_lock);',
     '    if (!app) RET(1, 4);\n'
     '    if (key && val && skyrim_light_write_is_echo(GSTR(ARG(3)), app, key, val)) RET(1, 4);\n'
     '    pthread_mutex_lock(&g_ini_lock);',
     "lightweight write echo"),
)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"{label}: expected one patch anchor; found {count}")
    return text.replace(old, new, 1)


def patch_shims(text: str) -> str:
    text = replace_once(text, INCLUDE[0], INCLUDE[1], "lightweight include")
    for old, new, label in HUNKS:
        text = replace_once(text, old, new, label)
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shims", type=Path, help="staged skyrim_shims.c (already prepared by prepare_runtime.py)")
    args = parser.parse_args()
    path = args.shims.resolve(strict=True)
    if path.is_symlink():
        raise ValueError(f"refusing to patch a symlink: {path}")
    original = path.read_text(encoding="utf-8")
    if "lightweight_core.h" in original:
        raise ValueError("lightweight mode is already applied to this file")
    path.write_text(patch_shims(original), encoding="utf-8", newline="\n")
    print("Applied lightweight mode to the staged ini functions.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"lightweight staging failed: {error}", file=sys.stderr)
        raise SystemExit(1)
