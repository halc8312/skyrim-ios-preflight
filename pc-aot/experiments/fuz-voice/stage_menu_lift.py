#!/usr/bin/env python3
"""Stage the owned Classic selection, subtitle queue and response lifecycle."""
import hashlib

from stage_lift import ANCHOR_HASH, ANCHOR_MARKER, UNIT_HASH, check_generation
from stage_resource_lift import extract_function

MENU_UNITS = {
    "exe_100.c": UNIT_HASH,
    "exe_101.c": "8ec90cddea6c81f16123a5f9fbc7ae7f9059295712fac1383565ce2abdeb697b",
    "exe_189.c": "4c4972e9ba7a0647d3c5ce49c11268a4e3e28b5b44ee89ec7d7a5ceb5fb9a243",
}
QUEUE_SUPPORT = {
    "exe_00.c": ("cd5e4755a7a1efac8839759c81b4f3b66a6bb8e34e8947dc612546af876e46e3", "402C00"),
    "exe_07.c": ("861ef895cc581dda062becd629a437955b249b279c7b3232514e9c4fdc84f65a", "431B00"),
}
SOURCE_HEADER = b'#include "funcs_exe.h"\n#include "lift_prelude.h"\n'
HOST_HEADER = b'#include "lift_menu_seam.h"\n'
QUEUE_GATE = b"if (f.zf) goto L_89817F;"
VOICE_MARKER = ANCHOR_MARKER
DELETE_MARKER = b"0x672EC1u"
SELECTED_MARKER = b"0x675CAAu"


def replace_one(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise ValueError(f"Expected one reviewed menu anchor {anchor!r}.")
    return source.replace(anchor, replacement, 1)


def call_line(function, marker):
    matches = [line for line in function.splitlines(keepends=True) if marker in line]
    if len(matches) != 1:
        raise ValueError(f"Expected one original call carrying {marker!r}.")
    return matches[0]


def entry_guard(function):
    end = function.index(b"(CPU *c) {\n") + len(b"(CPU *c) {\n")
    return function[:end] + b"    skyrim_fuz_host_forget_response(c->r[1]);\n" + function[end:]


def prepare_responses(source):
    voice = extract_function(source, "6731A0")
    original = call_line(voice, VOICE_MARKER)
    if hashlib.sha256(original.rstrip(b"\n")).hexdigest() != ANCHOR_HASH:
        raise ValueError("The original Classic voice line changed.")
    observed = (b"    {\n"
                b"    SkyrimFuzLiftChoice chosen = {0};\n"
                b"    skyrim_fuz_host_choose_menu_voice(esp, esi, &chosen);\n"
                + original +
                b"    skyrim_fuz_host_note_menu_assignment(esi, &chosen);\n"
                b"    }\n")
    staged = replace_one(source, voice, replace_one(entry_guard(voice), original, observed))
    functions = []
    for name in ("673170", "673030"):
        original_body = extract_function(staged, name)
        guarded = entry_guard(original_body)
        staged = replace_one(staged, original_body, guarded)
        if name == "673170":
            functions.append(guarded)
    release = extract_function(staged, "672EA0")
    original_call = call_line(release, DELETE_MARKER)
    guarded_release = replace_one(release, original_call,
                                  b"    skyrim_fuz_host_forget_response(esi);\n" + original_call)
    staged = replace_one(staged, release, guarded_release)
    functions.append(guarded_release)

    # Exercise the exact original assignment call and carry its stack ABI.
    # Production-style staging above additionally retains the full constructor.
    signature = voice.index(b"(CPU *c) {\n") + len(b"(CPU *c) {\n")
    registers = voice[signature:voice.index(b"L_6731A0:\n")]
    probe = (b"\nvoid skyrim_fuz_probe_voice_assignment(CPU *c) {\n" +
             registers + observed + b"    SPILL();\n}\n")
    functions.append(probe)
    return HOST_HEADER + staged, b"".join(functions)


def prepare_selection(source):
    selected = extract_function(source, "6758A0")
    original = call_line(selected, SELECTED_MARKER)
    adjusted = (b"    SkyrimFuzMenuScope response_scope = skyrim_fuz_host_capture_menu(ebx, ecx, M32(esp));\n" +
                replace_one(original, b"f_8980D0(c);", b"skyrim_fuz_host_queue_selected(c, &response_scope);"))
    staged = replace_one(source, selected, replace_one(selected, original, adjusted))

    # Keep the whole reviewed tail that selects and passes this one response.
    # The fixture stops when the lifted function enters its next phase.
    signature = selected.index(b"(CPU *c) {\n") + len(b"(CPU *c) {\n")
    registers = selected[signature:selected.index(b"L_6758A0:\n")]
    begin, end = selected.index(b"L_675C73:\n"), selected.index(b"L_675CBB:\n")
    path = selected[begin:end]
    end_probe = b"L_675CBB:\nL_675CCC:\n    SPILL();\n}\n"
    base = (b"\nvoid skyrim_fuz_probe_original_selection(CPU *c) {\n" + registers +
            b"    goto L_675C73;\n" + path + end_probe)
    probe = (b"\nvoid skyrim_fuz_probe_selected_response(CPU *c) {\n" + registers +
             b"    goto L_675C73;\n" + replace_one(path, original, adjusted) + end_probe)
    return HOST_HEADER + staged, base + probe


def prepare_queue(source):
    queue = extract_function(source, "8980D0")
    observed = replace_one(
        queue, b"void f_8980D0(CPU *c) {\n",
        b"static void fuz_menu_queue(CPU *c, const SkyrimFuzMenuScope *selection) {\n"
        b"    const uint32_t entry_stack = c->r[4];\n",
    )
    observed = replace_one(
        observed, QUEUE_GATE,
        b"if (f.zf && (!selection || !skyrim_fuz_host_force_menu(selection,\n"
        b"                           M32(entry_stack), M32(entry_stack + 4)))) goto L_89817F;",
    )
    observed += (b"\nvoid f_8980D0(CPU *c) { fuz_menu_queue(c, NULL); }\n"
                 b"void skyrim_fuz_host_queue_selected(CPU *c, const SkyrimFuzMenuScope *selection) {\n"
                 b"    fuz_menu_queue(c, selection);\n}\n")
    return HOST_HEADER + replace_one(source, queue, observed), observed


def prepare_menu(root):
    check_generation(root)
    stages, fixtures = {}, []
    preparers = (prepare_responses, prepare_selection, prepare_queue)
    for (name, digest), prepare in zip(MENU_UNITS.items(), preparers):
        path = root / "gen" / name
        contents = path.read_bytes()
        if hashlib.sha256(contents).hexdigest() != digest:
            raise ValueError(f"{name}: different from the reviewed Classic menu lift.")
        staged, fixture = prepare(contents)
        stages[name] = staged
        fixtures.append(fixture)
    for name, (digest, function) in QUEUE_SUPPORT.items():
        contents = (root / "gen" / name).read_bytes()
        if hashlib.sha256(contents).hexdigest() != digest:
            raise ValueError(f"{name}: different from the reviewed Classic menu queue support.")
        fixtures.append(extract_function(contents, function))
    return stages, SOURCE_HEADER + HOST_HEADER + b"".join(fixtures)
