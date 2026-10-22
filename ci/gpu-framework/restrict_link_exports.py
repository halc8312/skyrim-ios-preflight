#!/usr/bin/env python3
"""Restrict four pinned objc2 bookkeeping statics at the final Darwin link.

These are private define_class! statics, not Objective-C runtime class symbols.
Keep all definitions/references; only remove their dynamic export-list entries.
Do not use an unexported list: Apple's ld rejects combining it with Rust's list.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

INTERNAL = frozenset({
    "___CLASS_RawWindowMetalLayer", "___IVAR_OFFSET_RawWindowMetalLayer",
    "___DROP_FLAG_OFFSET_RawWindowMetalLayer", "___REGISTER_CLASS_RawWindowMetalLayer",
})
PROVENANCE = {
    "raw-window-metal": {"version": "1.1.0", "checksum": "40d213455a5f1dc59214213c7330e074ddf8114c9a42411eb890c767357ce135",
        "source_commit": "1b92ba84006b2d924e066d42ed77aaa58c0b4c62", "file": "src/observer.rs"},
    "objc2": {"version": "0.6.4", "checksum": "3a12a8ed07aefc768292f076dc3ac8c48f3781c8f2d5851dd3d98950e8c5a89f",
        "source_commit": "8852b424193ca41602281b3d7540d7c8ed51e49a", "file": "crates/objc2/src/macros/define_class.rs"},
}


def restrict(text: str) -> tuple[str, dict]:
    symbols = [line.strip() for line in text.splitlines() if line.strip()]
    if len(symbols) != len(set(symbols)):
        raise ValueError("duplicate linker export names")
    extras = {symbol for symbol in symbols if not re.fullmatch(r"_wgpu[A-Za-z0-9_]+", symbol)}
    if extras != INTERNAL:
        raise ValueError(f"unexpected export set; only pinned objc2 bookkeeping can be hidden: {sorted(extras)}")
    public = [symbol for symbol in symbols if symbol not in INTERNAL]
    if "_wgpuGetVersion" not in public or "_wgpuCreateInstance" not in public:
        raise ValueError("missing essential public C API")
    return "".join(symbol + "\n" for symbol in public), {
        "input_count": len(symbols), "output_count": len(public),
        "hidden_exact_symbols": sorted(INTERNAL), "source_provenance": PROVENANCE,
        "internal_definitions_or_references_removed": False,
        "gpu_runtime_verified": False,
    }


def rewrite(arguments: list[str], directory: Path) -> list[str]:
    # Rust currently passes one regular exported_symbols_list via the cc driver.
    # Refuse unknown forms instead of silently linking with broader exports.
    if any(arg.startswith("@") and not arg.startswith(("@rpath/", "@loader_path/", "@executable_path/")) for arg in arguments):
        raise ValueError("linker response files require a separately audited parser")
    occurrences: list[tuple[int, str, Path]] = []
    for index, argument in enumerate(arguments):
        if argument.startswith("-Wl,-exported_symbols_list,"):
            occurrences.append((index, "combined", Path(argument.split(",", 2)[2])))
        elif argument in ("-Wl,-exported_symbols_list", "-exported_symbols_list") and not (index and arguments[index - 1] == "-Xlinker"):
            if index + 1 >= len(arguments):
                raise ValueError("missing export list path")
            next_arg = arguments[index + 1]
            occurrences.append((index + 1, "wrapped" if next_arg.startswith("-Wl,") else "plain",
                                Path(next_arg[4:] if next_arg.startswith("-Wl,") else next_arg)))
        elif argument == "-Xlinker" and index + 1 < len(arguments) and arguments[index + 1] == "-exported_symbols_list":
            if index + 3 >= len(arguments) or arguments[index + 2] != "-Xlinker":
                raise ValueError("unknown Xlinker export-list form")
            occurrences.append((index + 3, "plain", Path(arguments[index + 3])))
    # The Xlinker sequence's standalone token is handled only by that sequence.
    occurrences = list(dict.fromkeys(occurrences))
    if len(occurrences) != 1:
        raise ValueError("expected exactly one Darwin exported_symbols_list")
    index, style, original = occurrences[0]
    text, report = restrict(original.read_text(encoding="utf-8"))
    directory.mkdir(parents=True, exist_ok=True)
    stem = "exports-" + uuid.uuid4().hex
    filtered = directory / (stem + ".txt")
    with filtered.open("x", encoding="utf-8") as output:
        output.write(text)
    with (directory / (stem + ".json")).open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    result = list(arguments)
    result[index] = {"combined": "-Wl,-exported_symbols_list,", "wrapped": "-Wl,", "plain": ""}[style] + str(filtered)
    return result


def main() -> None:
    linker = Path(os.environ["WGPU_REAL_LINKER"])
    if not linker.is_absolute() or not linker.is_file():
        raise ValueError("an absolute official Xcode clang linker is required")
    arguments = rewrite(sys.argv[1:], Path(os.environ["WGPU_LINK_POLICY_DIR"]))
    raise SystemExit(subprocess.call([str(linker), *arguments]))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        print(f"Export restriction failed: {error}", file=sys.stderr)
        raise SystemExit(2)
