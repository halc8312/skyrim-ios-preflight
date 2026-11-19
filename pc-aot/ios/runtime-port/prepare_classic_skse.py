#!/usr/bin/env python3
"""Apply the Classic-only public adapters to a prepared runtime overlay."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile

from prepare_runtime import replace_once

PINNED = {
    "skse_compat.h": "306883571cc167ee22636905a45ce3af95e779d9f6cefbb414a394c4764c9eaa",
    "skse_compat_values.c": "45ff5d9de3a27cf398a19bd55599031511e7334a58ca312b64767553105383e1",
    "skse_system.c": "61ce22b226c98264382722d0bc54d5835b32ae4821466d1f6a5b91e7af04b07c",
}


def adapt_ini(name, text):
    if hashlib.sha256(text.encode()).hexdigest() != PINNED[name]:
        raise ValueError(f"{name}: different from the reviewed Classic runtime")
    if name == "skse_compat.h":
        return replace_once(text, "S_INI_FLOAT,S_INI_INT,S_SET_INI_FLOAT,",
                            "S_INI_FLOAT,S_INI_INT,S_SET_INI_FLOAT,S_INI_BOOL,S_INI_STRING,", name)
    if name == "skse_compat_values.c":
        row = '    F("Utility","GetINIInt",3,1,2,0,0,0,S_INI_INT),'
        return replace_once(text, row, row
            + '\n    F("Utility","GetINIBool",5,1,2,0,0,0,S_INI_BOOL),'
            + '\n    F("Utility","GetINIString",2,1,2,0,0,0,S_INI_STRING),', name)
    entry = "int skse_system_run("
    text = replace_once(text, entry, '#include "classic_skse_ini.h"\n' + entry, name)
    start = text.index("    if(op>=S_INI_FLOAT && op<=S_SET_INI_FLOAT)")
    end = text.index("    if(op==1300)", start)
    return text[:start] + (
        "    if (op >= S_INI_FLOAT && op <= S_INI_STRING) {\n"
        "        skse_classic_ini_dispatch(c, op, args, result);\n"
        "        return 1;\n"
        "    }\n") + text[end:]


def prepare(source, runtime):
    from classic_skse_translation import adapt as adapt_translation

    source = (source / "runtime").resolve(strict=True)
    runtime = runtime.resolve(strict=True)
    if runtime == source or runtime.is_relative_to(source) or source.is_relative_to(runtime):
        raise ValueError("Classic adapters require a staged runtime outside the vendor source")
    changes = []
    for name in (*PINNED, "skse_translation.c"):
        original = (source / name).read_text(encoding="utf-8")
        new = adapt_translation(original) if name == "skse_translation.c" else adapt_ini(name, original)
        target = runtime / name
        if target.is_symlink() or target.samefile(source / name):
            raise ValueError(f"{name}: expected an independent staged copy")
        old = target.read_text(encoding="utf-8")
        if old != new:
            if old != original:
                raise ValueError(f"{name}: unrecognized contents in staged runtime")
            changes.append((target, new))
    # Validate every input before publishing and preserve intact files on a failed write.
    # Each individual replacement is atomic and a subsequent run can finish the rest.
    for target, new in changes:
        with tempfile.NamedTemporaryFile(dir=runtime, delete=False) as stream:
            temporary = Path(stream.name)
        try:
            temporary.write_text(new, encoding="utf-8", newline="\n")
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
    return len(changes)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[2] / "vendor/skydream")
    args = parser.parse_args()
    try:
        print(f"Classic runtime validated; updated {prepare(args.source, args.runtime)} staged files.")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Classic runtime preparation failed: {error}\n")
