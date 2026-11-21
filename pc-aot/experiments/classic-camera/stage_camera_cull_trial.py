"""Stage one-shot cull-witness callbacks in a private owned Classic lift."""
from hashlib import sha256
import re

UNITS = {
    "exe_106.c": "d606d487f73c4eee34d00dd37788b5ed8cf4962966d9150436d34322df0c231a",
    "exe_107.c": "c0a140d6fe5a3c86e3f0c970e669c8911f787ce14a72ff7da352e1c0c82e85d7",
    "exe_275.c": "c980cb667ac15d7f1bbe6df18de61cb550dd8319719eac53e5f74da4cd33a716",
    "exe_359.c": "5ccfc975c4b0aabdb720e3b179e90e2d41dd92d1151fb0ac9466aed6f34343cb",
}
DECLARATION = b'#include "camera_cull_trial.h"\n'
CALLER = b"    const uint32_t skyrim_cull_caller = M32(c->r[4]);\n"
PREPARE = b"    const bool skyrim_cull_frame = skyrim_camera_cull_prepare(esi);\n"
PREPARED = b"    skyrim_camera_cull_prepared(skyrim_cull_frame);\n"
FINISH = b"    skyrim_camera_cull_finish(skyrim_cull_frame);\n"

PREPARE_CALL = b"    esp -= 4; M32(esp) = 0x69B919u; SPILL(); f_698800(c); RELOAD();\n"
RENDER_CALL = b"    esp -= 4; M32(esp) = 0x69B96Fu; SPILL(); f_69A9F0(c); RELOAD();\n"
GATES = re.compile(rb"(    \(void\)logic8\(\(uint8_t\)\(TL8\(\(uint32_t\)\(([a-z]{3}) \+ 0x98u\)\)"
                   rb" & \(uint8_t\)0x1u\), &f\);\n    LD_FENCE\(\);\n)")
PRODUCER = b"    skyrim_camera_cull_producer(skyrim_cull_caller, %s, f.zf);\n"
DECISION = b"    skyrim_camera_cull_decision(M32(esp), ecx, f.zf);\n"
DISPATCH = b"    const bool skyrim_cull_list%u = skyrim_camera_cull_dispatch(skyrim_cull_caller, ecx, %u);\n"
DISPATCHED = b"    skyrim_camera_cull_dispatched(skyrim_cull_list%u);\n"


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError("A reviewed camera cull anchor is missing or ambiguous")
    return source.replace(before, after, 1)


def frame(function):
    function = replace_once(function, PREPARE_CALL, PREPARE + PREPARE_CALL + PREPARED)
    return replace_once(function, RENDER_CALL, RENDER_CALL + FINISH)


def producer(function):
    def gate(match):
        return match[1] + PRODUCER % match[2]

    result, count = GATES.subn(gate, function)
    if count != 19:
        raise ValueError("The reviewed producer app-cull tests have changed")
    return result


def decision(function):
    anchor = b"    if (!f.zf) goto L_AAF44A;\n"
    return replace_once(function, anchor, DECISION + anchor)


def dispatch(function):
    for stage, return_site in ((1, b"CB9E99"), (2, b"CB9EB3")):
        call = b"    esp -= 4; M32(esp) = 0x" + return_site + b"u; SPILL(); f_C8D8D0(c); RELOAD();\n"
        function = replace_once(function, call, (DISPATCH % (stage, stage)) + call + (DISPATCHED % stage))
    return function


def stage_camera_cull_trial(unit, source, *, runtime_base=None):
    if unit not in UNITS or sha256(source).hexdigest() != UNITS[unit]:
        raise ValueError("Camera cull trial unit differs from the reviewed Classic lift")
    function, stage, capture_caller = {
        "exe_106.c": (b"694FD0", producer, True),
        "exe_107.c": (b"69B810", frame, False),
        "exe_275.c": (b"AAF430", decision, False),
        "exe_359.c": (b"CB9E20", dispatch, True),
    }[unit]
    entry = b"\nvoid f_" + function + b"(CPU *c) {\n"
    if source.count(entry) != 1:
        raise ValueError("The reviewed camera cull function is missing or ambiguous")
    start = source.index(entry)
    end = source.index(b"\n}\n", start) + len(b"\n}\n")
    original = source[start:end]
    changed = stage(original)
    if capture_caller:
        changed = replace_once(changed, entry, entry + CALLER)
    if runtime_base is not None:
        return b'#include "PcGuestModHooksABI.h"\n' + replace_once(runtime_base, original, runtime_callbacks(unit, changed))
    return DECLARATION + source[:start] + changed + source[end:]


def runtime_callbacks(unit, body):
    notify = b"skyrim_camera_cull_guest_notify"
    active = (b"skyrim_camera_cull_guest_selected && "
              b"__atomic_load_n(&skyrim_camera_cull_guest_frames, __ATOMIC_RELAXED)")
    if unit == "exe_107.c":
        body = replace_once(body, b"skyrim_camera_cull_prepare(esi)",
                            b"(skyrim_camera_cull_guest_selected && " + notify +
                            b"(SkyrimCameraCullPrepare, esi, 0, 0))")
        for name, notice in ((b"prepared", b"Prepared"), (b"finish", b"Finish")):
            body = replace_once(body, b"skyrim_camera_cull_" + name + b"(skyrim_cull_frame);",
                                b"if (skyrim_cull_frame) " + notify + b"(SkyrimCameraCull" + notice + b", 0, 0, 0);")
    elif unit == "exe_106.c":
        capture = b"    const bool skyrim_cull_active = " + active + b";\n"
        body = replace_once(body, CALLER, CALLER + capture)
        body, count = re.subn(rb"skyrim_camera_cull_producer\(skyrim_cull_caller, ([a-z]{3}), f.zf\);",
                              b"if (skyrim_cull_active) " + notify + rb"(SkyrimCameraCullProducer, skyrim_cull_caller, \1, f.zf);",
                              body)
        if count != 19:
            raise ValueError("Expected the reviewed camera producer observations")
    elif unit == "exe_275.c":
        body = replace_once(body, DECISION, b"    if (" + active + b") " + notify +
                            b"(SkyrimCameraCullDecision, M32(esp), ecx, f.zf);\n")
    elif unit == "exe_359.c":
        for stage in (1, 2):
            body = replace_once(body, DISPATCH % (stage, stage), b"    const bool skyrim_cull_list%u = " % stage +
                                active + b" && " + notify + b"(SkyrimCameraCullDispatch, skyrim_cull_caller, ecx, %u);\n" % stage)
            body = replace_once(body, DISPATCHED % stage, b"    if (skyrim_cull_list%u) " % stage +
                                notify + b"(SkyrimCameraCullDispatched, 0, 0, 0);\n")
    return body
