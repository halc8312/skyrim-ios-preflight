#!/usr/bin/env python3
"""Apply the Classic-only public adapters to a prepared runtime overlay."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile

from prepare_runtime import replace_once

PINNED = {
    "skse_compat.c": "ab9e96a3b330942c8f5623dd18af96889be243dcd3ae7fb9bbde711458bc96b4",
    "skse_compat.h": "306883571cc167ee22636905a45ce3af95e779d9f6cefbb414a394c4764c9eaa",
    "skse_compat_values.c": "45ff5d9de3a27cf398a19bd55599031511e7334a58ca312b64767553105383e1",
    "skse_system.c": "61ce22b226c98264382722d0bc54d5835b32ae4821466d1f6a5b91e7af04b07c",
}


MOD_QUERIES = (
    ("GetModCount", "S_MOD_COUNT", 3, ()),
    ("GetModByName", "S_MOD_FIND", 3, (2,)),
    ("GetModName", "S_MOD_NAME", 2, (3,)),
    ("GetModAuthor", "S_MOD_AUTHOR", 2, (3,)),
    ("GetModDescription", "S_MOD_DESCRIPTION", 2, (3,)),
    ("GetModDependencyCount", "S_MOD_MASTER_COUNT", 3, (3,)),
    ("GetNthModDependency", "S_MOD_NTH_MASTER", 3, (3, 3)),
)


def adapt_ini(name, text):
    if hashlib.sha256(text.encode()).hexdigest() != PINNED[name]:
        raise ValueError(f"{name}: different from the reviewed Classic runtime")
    if name == "skse_compat.c":
        return text
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


def adapt_mod_queries(name, text):
    if name == "skse_compat.c":
        registration = "void skse_register_natives(CPU *c, uint32_t registry) {\n"
        text = replace_once(text, registration + "    unsigned count=0;",
                            registration + "    unsigned count=0, passed=0;", name)
        text = replace_once(text, '            probe_native(c,object,fn);',
                            '        {\n            probe_native(c,object,fn);\n            ++passed;\n        }', name)
        return replace_once(text, '    LOG("SKSE compatibility: registered %u',
                            '    fprintf(stderr, "IOS_CLASSIC_VM registered=%u primitive_passed=%u\\n", count, passed);\n'
                            '    LOG("SKSE compatibility: registered %u', name)
    if name == "skse_compat.h":
        operations = [row[1] for row in MOD_QUERIES]
        operations[0] += "=1310"  # leave existing system operation numbers unchanged
        return replace_once(text, " S_METHOD=0x8000", " " + ",".join(operations)
                            + ",\n S_METHOD=0x8000", name)
    if name == "skse_compat_values.c":
        rows = []
        for function, op, result, args in MOD_QUERIES:
            parameters = [result, len(args), *args, *([0] * (4 - len(args)))]
            rows.append(f'    F("Game","{function}",' + ",".join(map(str, parameters)) + f",{op}),")
        return replace_once(text, "\n};\n#undef F", "\n" + "\n".join(rows) + "\n};\n#undef F", name)
    text = replace_once(text, '#include "classic_skse_ini.h"',
                        '#include "classic_skse_ini.h"\n#include "classic_skse_mod_list.h"', name)
    return replace_once(text, "    if(op==1300)", (
        "    if (op >= S_MOD_COUNT && op <= S_MOD_NTH_MASTER) {\n"
        "        if (skse_classic_mod_query(c, op, args, result)) return 1;\n"
        '        LOG("SKSE %s.%s: plugin list unavailable or inconsistent",fn->class_name,fn->name);\n'
        "        return 0;\n"
        "    }\n"
        "    if(op==1300)"), name)


def adapt_skyui_selftest(text):
    # The VM probe verifies dispatch against the same configured version as
    # Papyrus. Keep all the normal type and value checks enabled in both modes.
    name = "skse_compat.c"
    text = replace_once(text, "static void run_native(CPU *c) {", r"""static uint32_t skyui_version_value(const SkseNative *fn, uint32_t value) {
    if (strcmp(fn->class_name, "SKSE") == 0 && skse_skyui_enabled()) {
        if (strcmp(fn->name, "GetVersion") == 0) return 1;
        if (strcmp(fn->name, "GetVersionMinor") == 0) return 7;
        if (strcmp(fn->name, "GetVersionBeta") == 0) return 3;
        if (strcmp(fn->name, "GetVersionRelease") == 0) return 48;
    }
    return value;
}

static void run_native(CPU *c) {""", name)
    text = replace_once(text, r"""    if(skse_skyui_enabled() && !strcmp(fn->class_name,"SKSE")) {
        if(!strcmp(fn->name,"GetVersion")) value=1;
        if(!strcmp(fn->name,"GetVersionMinor")) value=7;
        if(!strcmp(fn->name,"GetVersionBeta")) value=3;
        if(!strcmp(fn->name,"GetVersionRelease")) value=48;
    }""", "    value = skyui_version_value(fn, value);", name)
    return replace_once(text, "    uint32_t runargs[]={0,0,0,result,state};",
                        "    expected = skyui_version_value(fn, expected);\n"
                        "    uint32_t runargs[]={0,0,0,result,state};", name)


def prepare(source, runtime):
    from classic_skse_translation import adapt as adapt_translation

    source = (source / "runtime").resolve(strict=True)
    runtime = runtime.resolve(strict=True)
    if runtime == source or runtime.is_relative_to(source) or source.is_relative_to(runtime):
        raise ValueError("Classic adapters require a staged runtime outside the vendor source")
    changes = []
    for name in (*PINNED, "skse_translation.c"):
        original = (source / name).read_text(encoding="utf-8")
        previous = original if name == "skse_translation.c" else adapt_ini(name, original)
        adapted = adapt_translation(original) if name == "skse_translation.c" else adapt_mod_queries(name, previous)
        new = adapt_skyui_selftest(adapted) if name == "skse_compat.c" else adapted
        target = runtime / name
        if target.is_symlink() or target.samefile(source / name):
            raise ValueError(f"{name}: expected an independent staged copy")
        old = target.read_text(encoding="utf-8")
        if old != new:
            if old not in (original, previous, adapted):
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
