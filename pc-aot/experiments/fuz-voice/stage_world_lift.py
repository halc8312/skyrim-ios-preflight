#!/usr/bin/env python3
"""Stage a reviewed world subtitle queue admission, copy and setting gate."""
import hashlib

from stage_lift import UNIT_HASH, check_generation
from stage_menu_lift import SOURCE_HEADER, call_line, prepare_responses, replace_one
from stage_resource_lift import extract_function

HOST_HEADER = b'#include "lift_world_seam.h"\n'
WORLD_UNITS = {
    "exe_35.c": "ca8f181c1f0ba88c727f1e406308336248400fa68e4b157318daf51fb389fec4",
    "exe_188.c": "469401623fe725326dc89286db07111e1862b1d96ce8a4c1fab4f96cf74a097b",
    "exe_106.c": "d606d487f73c4eee34d00dd37788b5ed8cf4962966d9150436d34322df0c231a",
}
SUPPORT = {
    "exe_00.c": (
        "cd5e4755a7a1efac8839759c81b4f3b66a6bb8e34e8947dc612546af876e46e3", ("402C00", "4016B0", "4016C0"),
    ),
    "exe_259.c": (
        "842247dde6c84c461377e75ba002b3b7dc1779bd33524489176820c9477b02f0", ("A49B80", "A49D90", "A49DC0", "A4A070"),
    ),
}


def verified_source(root, name, digest):
    data = (root / "gen" / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"{name}: different from the reviewed Classic world lift.")
    return data


def excerpt(name, address, function, start, finish, tails):
    signature = function.index(b"{\n") + 2
    label = b"L_" + address + b":\n"
    registers = function[signature:function.index(label)]
    initial = b"L_" + start + b":\n"
    final = b"L_" + finish + b":\n"
    path = function[function.index(initial):function.index(final)]
    return b"\nvoid " + name + b"(CPU *c) {\n" + registers + b"    goto L_" + start + b";\n" + path + tails + b"}\n"


def patch_source_response(source):
    function = extract_function(source, "4DFBF0")
    start = call_line(function, b"0x4DFD0Bu")
    end = call_line(function, b"0x4DFDDDu")
    local = b"    SkyrimFuzWorldScope world_scope = {0};\n"
    witnessed = b"    world_scope = skyrim_fuz_host_capture_world(ebp, M32(esp + 0x10u));\n" + start
    guided = replace_one(end, b"f_892CE0(c);", b"skyrim_fuz_host_queue_world(c, &world_scope);")
    observed = replace_one(function, start, witnessed)
    observed = replace_one(observed, end, guided)
    observed = replace_one(observed, b"L_4DFBF0:\n", local + b"L_4DFBF0:\n")
    stage = replace_one(source, function, observed)
    probe = excerpt(b"skyrim_fuz_probe_world_source", b"4DFBF0", observed, b"4DFCA3", b"4DFDE1",
                    b"L_4DFDE1:\n    SPILL();\n")
    return HOST_HEADER + stage, probe


def separate_entry(function, address, implementation, setup):
    original = b"void f_" + address + b"(CPU *c) {\n"
    return replace_one(function, original, b"static void " + implementation +
                       b"(CPU *c, const SkyrimFuzWorldScope *scope) {\n" + setup)


def patch_world_queue(source):
    append = extract_function(source, "892C90")
    observed_append = separate_entry(append, b"892C90", b"fuz_copy_chatter",
        b"    const uint32_t input_record = M32(c->r[4] + 4);\n")
    copied = call_line(observed_append, b"0x892CCCu")
    observed_append = replace_one(observed_append, copied, copied +
        b"    skyrim_fuz_host_world_after_copy(esi, edi, input_record, scope);\n")
    observed_append += b"\nvoid f_892C90(CPU *c) { fuz_copy_chatter(c, NULL); }\n"

    admission = extract_function(source, "892CE0")
    observed_admission = separate_entry(admission, b"892CE0", b"fuz_admit_chatter",
        b"    const uint32_t entry_stack = c->r[4];\n"
        b"    const SkyrimFuzWorldScope *selected = scope && skyrim_fuz_host_admit_world(\n"
        b"        scope, M32(entry_stack), M32(entry_stack + 8)) ? scope : NULL;\n")
    begin = observed_admission.index(b"L_892D21:\n")
    end = observed_admission.index(b"L_892D2A:\n")
    gate = observed_admission[begin:end]
    observed_admission = replace_one(observed_admission, gate, replace_one(
        gate, b"if (f.zf) goto L_892D85;", b"if (f.zf && !selected) goto L_892D85;"))
    observed_admission = replace_one(observed_admission, b"f_892C90(c);", b"fuz_copy_chatter(c, selected);")
    observed_admission += (b"\nvoid f_892CE0(CPU *c) { fuz_admit_chatter(c, NULL); }\n"
                            b"void skyrim_fuz_host_queue_world(CPU *c, const SkyrimFuzWorldScope *scope) {\n"
                            b"    fuz_admit_chatter(c, scope);\n}\n")

    display = extract_function(source, "892AB0")
    observed_display = replace_one(display, b"if (f.zf) goto L_892C0B;",
        b"if (f.zf && !skyrim_fuz_host_world_general(esi + 4, ebp)) goto L_892C0B;")
    stage = replace_one(source, append, observed_append)
    stage = replace_one(stage, admission, observed_admission)
    stage = replace_one(stage, display, observed_display)

    display_result = (b"L_892C0B:\n    eax = 0; goto done;\n"
                      b"L_892B2A:\n    eax = 1; goto done;\n"
                      b"L_892C44:\n    eax = 2; goto done;\n"
                      b"L_892C56:\n    eax = 3;\n"
                      b"done:\n    SPILL();\n")
    functions = [extract_function(source, "8929A0"), extract_function(source, "8929E0"),
                 observed_append, observed_admission, extract_function(source, "892DA0")]
    for name, body in ((b"skyrim_fuz_probe_original_world_display", display),
                       (b"skyrim_fuz_probe_world_display", observed_display)):
        functions.append(excerpt(name, b"892AB0", body, b"892AF2", b"892B2A", display_result))

    for func, marker in (("892E50", b"0x892F55u"), ("893070", b"0x893173u")):
        old = extract_function(stage, func)
        line = call_line(old, marker)
        guard = b"    skyrim_fuz_host_release_chatter_title(M32(esp));\n" + line
        stage = replace_one(stage, old, replace_one(old, line, guard))
        signature = old.index(b"{\n") + 2
        registers = old[signature:old.index(b"L_" + func.encode("ascii") + b":\n")]
        functions.append(b"\nvoid skyrim_fuz_probe_world_release_" + func.encode("ascii") + b"(CPU *c) {\n" +
                         registers + guard + b"    SPILL();\n}\n")
    return HOST_HEADER + stage, b"".join(functions)


def patch_range_release(source):
    function = extract_function(source, "6963E0")
    line = call_line(function, b"0x696413u")
    guarded = replace_one(function, line, b"    skyrim_fuz_host_release_chatter_title(M32(esp));\n" + line)
    return HOST_HEADER + replace_one(source, function, guarded), guarded


def prepare_world(root):
    check_generation(root)
    stages, fragments = {}, [SOURCE_HEADER, HOST_HEADER]
    for name, patch in (("exe_35.c", patch_source_response), ("exe_188.c", patch_world_queue),
                        ("exe_106.c", patch_range_release)):
        staged, probe = patch(verified_source(root, name, WORLD_UNITS[name]))
        stages[name] = staged
        fragments.append(probe)
    stages["exe_100.c"], response = prepare_responses(verified_source(root, "exe_100.c", UNIT_HASH))
    fragments.append(response)
    for filename, (digest, routines) in SUPPORT.items():
        contents = verified_source(root, filename, digest)
        fragments.extend(extract_function(contents, name) for name in routines)
    return stages, b"".join(fragments)
