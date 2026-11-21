#!/usr/bin/env python3
"""Apply the Classic-only public adapters to a prepared runtime overlay."""
import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile

from prepare_runtime import replace_once

PINNED = {
    "skse_compat.c": "ab9e96a3b330942c8f5623dd18af96889be243dcd3ae7fb9bbde711458bc96b4",
    "skse_compat.h": "306883571cc167ee22636905a45ce3af95e779d9f6cefbb414a394c4764c9eaa",
    "skse_compat_values.c": "45ff5d9de3a27cf398a19bd55599031511e7334a58ca312b64767553105383e1",
    "skse_system.c": "61ce22b226c98264382722d0bc54d5835b32ae4821466d1f6a5b91e7af04b07c",
}

PRESENT_FILE = "platform_sdl.c"
PRESENT_HOOK = """#ifdef SKYDREAM_SKSE
    extern void skse_ui_pump(void); skse_ui_pump();
#endif
"""
TRIAL_PRESENT_HOOK = """#ifdef SKYDREAM_SKSE
    extern int pc_guest_classic_skse_active; /* selected before guest startup */
    extern void skse_ui_pump(void);
    if (pc_guest_classic_skse_active) skse_ui_pump();
#endif
"""


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


def adapt_present(text):
    # With the setting off, use the normal input and event path. In particular
    # do not install SKSE input registrations from the per-frame callback.
    if text.count(TRIAL_PRESENT_HOOK) == 1 and PRESENT_HOOK not in text:
        return text
    if TRIAL_PRESENT_HOOK in text:
        raise ValueError(f"{PRESENT_FILE}: duplicate Classic Present hooks")
    return replace_once(text, PRESENT_HOOK, TRIAL_PRESENT_HOOK, PRESENT_FILE)


def adapt_skyui_release_log(text):
    name = "skse_compat.c"
    anchor = "static uint32_t skyui_version_value("
    text = replace_once(text, anchor, r"""static _Thread_local unsigned skse_probe_depth;
static unsigned skse_release_reported;

static void note_skyui_release(const SkseNative *fn, uint32_t value) {
    if (skse_probe_depth || strcmp(fn->class_name, "SKSE") ||
        strcmp(fn->name, "GetVersionRelease") ||
        __atomic_exchange_n(&skse_release_reported, 1, __ATOMIC_RELAXED)) return;
    fprintf(stderr, "IOS_CLASSIC_RELEASE value=%u\n", (unsigned)value);
}

""" + anchor, name)
    text = replace_once(text, "    } else M32(result+4)=value;\n    RET(1,5);",
                        "    } else M32(result+4)=value;\n"
                        "    note_skyui_release(fn, value);\n    RET(1,5);", name)
    return replace_once(text,
        "    if(!skse_virtual(c,object,RUN_SLOT,5,runargs) || M32(result)!=fn->result)",
        "    ++skse_probe_depth;\n"
        "    uint32_t dispatched = skse_virtual(c,object,RUN_SLOT,5,runargs);\n"
        "    --skse_probe_depth;\n"
        "    if(!dispatched || M32(result)!=fn->result)", name)


def adapt_int0_probe(text):
    name = "skse_compat.c"
    registration = "void skse_register_natives(CPU *c, uint32_t registry) {\n"
    text = replace_once(text, registration,
                        '#ifdef SKYRIM_IOS_CLASSIC_INT0_PROBE\n#include "classic_papyrus_int0.c.in"\n#endif\n\n'
                        + registration, name)
    log = '    fprintf(stderr, "IOS_CLASSIC_VM registered='
    return replace_once(text, log,
                        "#ifdef SKYRIM_IOS_CLASSIC_INT0_PROBE\n"
                        "    int0_publish(c, registry, has);\n"
                        "#endif\n" + log, name)


def adapt_values_probe(text):
    name = "skse_compat.c"
    include = '#include "classic_papyrus_int0.c.in"\n'
    text = replace_once(text, include, include + '#include "classic_papyrus_values.c.in"\n', name)
    publish = "    int0_publish(c, registry, has);\n"
    return replace_once(text, publish, publish + "    classic_values_publish(c, registry, has);\n", name)


def adapt_vm_epoch(text):
    # Plan P3-1: report the registry to the VM generation (MOD hook builds only).
    name = "skse_compat.c"
    registration = "void skse_register_natives(CPU *c, uint32_t registry) {\n"
    return replace_once(text, registration + "    unsigned count=0, passed=0;",
                        '#ifdef SKYRIM_IOS_MOD_HOOKS\n#include "classic_vm_epoch.h"\n#endif\n'
                        + registration + "#ifdef SKYRIM_IOS_MOD_HOOKS\n"
                        "    skyrim_vm_epoch_registered(registry);\n#endif\n"
                        "    unsigned count=0, passed=0;", name)


MOD_EVENT_ROWS = (
    # Classic 1.7.3 PapyrusModEvent: static, NoWait. Types: 2 string, 3 int, 5 bool.
    ('Create', 3, (2,), 'S_MODEVENT_CREATE'),
    ('PushBool', 0, (3, 5), 'S_MODEVENT_PUSH_BOOL'),
    ('Send', 5, (3,), 'S_MODEVENT_SEND'),
    ('Release', 0, (3,), 'S_MODEVENT_RELEASE'),
)


def adapt_mod_event(name, text):
    # Plan P3-3: ModEvent statics for boolean packets (MOD hook builds only).
    if name == "skse_compat.h":
        return replace_once(text, " S_METHOD=0x8000",
                            " S_MODEVENT_CREATE=1330,S_MODEVENT_PUSH_BOOL,S_MODEVENT_SEND,S_MODEVENT_RELEASE,\n"
                            " S_METHOD=0x8000", name)
    if name == "skse_compat_values.c":
        rows = []
        for function, result, args, op in MOD_EVENT_ROWS:
            parameters = [result, len(args), *args, *([0] * (4 - len(args)))]
            rows.append(f'    F("ModEvent","{function}",' + ",".join(map(str, parameters)) + f",{op}),")
        return replace_once(text, "\n};\n#undef F", "\n#ifdef SKYRIM_IOS_MOD_HOOKS\n" + "\n".join(rows)
                            + "\n#endif\n};\n#undef F", name)
    if name != "skse_system.c":
        return text
    text = replace_once(text, '#include "classic_skse_mod_list.h"',
                        '#include "classic_skse_mod_list.h"\n#include "classic_skse_mod_event.h"', name)
    return replace_once(text, "    if(op==1300)", (
        "#ifdef SKYRIM_IOS_MOD_HOOKS\n"
        "    if (op >= S_MODEVENT_CREATE && op <= S_MODEVENT_RELEASE) return skse_classic_mod_event(c, op, args, registry, result);\n"
        "#endif\n"
        "    if(op==1300)"), name)


CONTROL_CLASSES = ("Form", "Alias", "ActiveMagicEffect")
INPUT_FILE = "skse_input.c"
INPUT_SHA256 = "e7545db20bf742f2981b3c73299ee8567cd3c2ddfe4e9f4c11ed55916094591d"
INPUT_GFX_CALL = '        skse_gfx_input_event(c,key,control?control:"",down);\n'
# experiments/classic-control-events: the mapped action and hold time of the same event.
INPUT_CONTROL_EDGE = ("#ifdef SKYRIM_IOS_MOD_HOOKS\n"
                      "        { extern void skyrim_classic_control_edge(CPU *, uint32_t, int, float);\n"
                      "          skyrim_classic_control_edge(c,M32(event+0x10),down,MF32(event+0x1c)); }\n"
                      "#endif\n")


def adapt_controls(name, text):
    # Plan P3-4: Classic control registration and events (MOD hook builds only).
    if name == "skse_compat.h":
        return replace_once(text, " S_METHOD=0x8000",
                            " S_REG_CONTROL=1340,S_UNREG_CONTROL,S_UNREG_CONTROLS,\n S_METHOD=0x8000", name)
    if name == "skse_compat_values.c":
        rows = []
        for cls in CONTROL_CLASSES:
            rows += [f'    F("{cls}","RegisterForControl",0,1,2,0,0,0,S_METHOD|S_REG_CONTROL),',
                     f'    F("{cls}","UnregisterForControl",0,1,2,0,0,0,S_METHOD|S_UNREG_CONTROL),',
                     f'    F("{cls}","UnregisterForAllControls",0,0,0,0,0,0,S_METHOD|S_UNREG_CONTROLS),']
        return replace_once(text, "\n#endif\n};\n#undef F", "\n" + "\n".join(rows) + "\n#endif\n};\n#undef F", name)
    if name != "skse_system.c":
        return text
    text = replace_once(text, '#include "classic_skse_mod_event.h"',
                        '#include "classic_skse_mod_event.h"\n#include "classic_skse_control.h"', name)
    return replace_once(text, "    if (op >= S_MODEVENT_CREATE && op <= S_MODEVENT_RELEASE)",
                        "    if (op >= S_REG_CONTROL && op <= S_UNREG_CONTROLS) return skse_classic_control(c, op, args, registry, base);\n"
                        "    if (op >= S_MODEVENT_CREATE && op <= S_MODEVENT_RELEASE)", name)


def adapt_input(text):
    if hashlib.sha256(text.encode()).hexdigest() != INPUT_SHA256:
        raise ValueError(f"{INPUT_FILE}: different from the reviewed Classic runtime")
    return replace_once(text, INPUT_GFX_CALL, INPUT_CONTROL_EDGE + INPUT_GFX_CALL, INPUT_FILE)


def prepare(source, runtime, save_observe=False, load_observe=False):
    if load_observe and not save_observe:
        raise ValueError("Fresh load preparation requires save observation")
    from classic_skse_extra import adapt as adapt_extra
    from classic_skse_translation import adapt as adapt_translation, adapt_profile_list

    source = (source / "runtime").resolve(strict=True)
    runtime = runtime.resolve(strict=True)
    if runtime == source or runtime.is_relative_to(source) or source.is_relative_to(runtime):
        raise ValueError("Classic adapters require a staged runtime outside the vendor source")
    changes = []
    for name in (*PINNED, "skse_translation.c"):
        original = (source / name).read_text(encoding="utf-8")
        previous = adapt_profile_list(original) if name == "skse_translation.c" else adapt_ini(name, original)
        adapted = adapt_translation(original) if name == "skse_translation.c" else adapt_mod_queries(name, previous)
        before_input_queries = adapt_skyui_selftest(adapted) if name == "skse_compat.c" else adapted
        input_queries = adapt_extra(name, before_input_queries)
        release_logged = adapt_skyui_release_log(input_queries) if name == "skse_compat.c" else input_queries
        int0 = adapt_int0_probe(release_logged) if name == "skse_compat.c" else release_logged
        values = adapt_values_probe(int0) if name == "skse_compat.c" else int0
        epoch = adapt_vm_epoch(values) if name == "skse_compat.c" else values
        events = adapt_mod_event(name, epoch)
        new = adapt_controls(name, events)
        target = runtime / name
        if target.is_symlink() or target.samefile(source / name):
            raise ValueError(f"{name}: expected an independent staged copy")
        old = target.read_text(encoding="utf-8")
        if old != new:
            stages = [original, previous, adapted, before_input_queries, input_queries, release_logged, int0, values,
                      epoch, events]
            if name == "skse_compat.c":
                # Earlier stages that already carry the VM generation call.
                for stage in stages[2:7]:
                    try:
                        stages.append(adapt_vm_epoch(stage))
                    except ValueError:
                        pass
            if old not in stages:
                raise ValueError(f"{name}: unrecognized contents in staged runtime")
            changes.append((target, new))
    # The input observer: pinned vendor source, one added edge report.
    original = (source / INPUT_FILE).read_text(encoding="utf-8")
    observed = adapt_input(original)
    target = runtime / INPUT_FILE
    if target.is_symlink() or target.samefile(source / INPUT_FILE):
        raise ValueError(f"{INPUT_FILE}: expected an independent staged copy")
    old = target.read_text(encoding="utf-8")
    if old != observed:
        if old != original:
            raise ValueError(f"{INPUT_FILE}: unrecognized contents in staged runtime")
        changes.append((target, observed))
    # The ordinary runtime adapter already transforms the SDL implementation.
    # Touch only its pinned Present anchor and leave the other edits intact.
    target = runtime / PRESENT_FILE
    if target.is_symlink() or target.samefile(source / PRESENT_FILE):
        raise ValueError(f"{PRESENT_FILE}: expected an independent staged copy")
    old = target.read_text(encoding="utf-8")
    gated = adapt_present(old)
    if old != gated:
        changes.append((target, gated))
    if save_observe:
        from prepare_runtime import patch_kernel
        save_directory = Path(__file__).resolve().parents[2] / "experiments" / "classic-runtime-save"
        sys.path.insert(0, str(save_directory))
        try:
            from stage_runtime import transform, verified_source
        finally:
            sys.path.pop(0)
        original = source / "kernel32.c"
        target = runtime / original.name
        if target.is_symlink() or target.samefile(original):
            raise ValueError("Save observation requires an independent staged runtime")
        base = patch_kernel(verified_source(original.read_bytes()))
        save_only = transform(base, app=True)
        with_load = transform(base, load_witness=True, load_clone=True, app=True)
        observed = with_load if load_observe else save_only
        old = target.read_text(encoding="utf-8")
        if old != observed:
            if old not in (base, save_only, with_load):
                raise ValueError("Save observation requires the reviewed iOS kernel preparation")
            changes.append((target, observed))
        # A guest longjmp can skip all of the save routine's ordinary returns.
        target = runtime / "setjmp.c"
        original = source / target.name
        if target.is_symlink() or target.samefile(original):
            raise ValueError("Save observation requires an independent staged runtime")
        base = original.read_text(encoding="utf-8")
        if hashlib.sha256(base.encode()).hexdigest() != "5f4ce702daa0d85e936d389cf9eb39faf8513527803415bd16e8adee69e7d46c":
            raise ValueError("Guest nonlocal return differs from the reviewed runtime")
        observed = replace_once(base, "    longjmp(*slot, 1);",
                                "    skyrim_save_app_retire(c);\n    longjmp(*slot, 1);", target.name)
        observed = '#include "mod_save_host.h"\n' + observed
        old = target.read_text(encoding="utf-8")
        if old != observed:
            if old != base:
                raise ValueError("Save observation requires the pinned guest nonlocal return")
            changes.append((target, observed))
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
    parser.add_argument("--save-observe", action="store_true")
    parser.add_argument("--load-observe", action="store_true")
    args = parser.parse_args()
    try:
        count = prepare(args.source, args.runtime, args.save_observe, args.load_observe)
        print(f"Classic runtime validated; updated {count} staged files.")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Classic runtime preparation failed: {error}\n")
