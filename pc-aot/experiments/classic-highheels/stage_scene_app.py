"""Add passive HighHeels diagnostics to the pinned stock scene calls."""

from hashlib import sha256

from stage_effect_magnitude import function
from stage_scene_refresh import replace_local_z
from stage_scene_witness import DEPENDENCIES, UNIT, UNITS, private_fixture as hosted_fixture


DIGESTS = {name: digest for name, digest, _ in UNITS if name in ("exe_34.c", "exe_275.c", "exe_276.c")}
DIGESTS.update({
    "exe_106.c": "d606d487f73c4eee34d00dd37788b5ed8cf4962966d9150436d34322df0c231a",
    "exe_108.c": "08bde439851e0994444a4edc2eeae2968a90159dd6d6b6723058b17743c20517",
})
HEADER = b'#include "PcGuestModHooksABI.h"\n'
STATE = b"__atomic_load_n(&skyrim_highheel_scene_guest_state, __ATOMIC_RELAXED)"
READY = STATE + b" == SkyrimHighHeelSceneReady"
ACTIVE = STATE + b" == SkyrimHighHeelSceneActive"
NOTIFY = b"skyrim_highheel_scene_guest_notify(c, SkyrimHighHeelScene"


def once(body, old, new):
    if body.count(old) != 1:
        raise ValueError("Expected one reviewed HighHeels scene call or field load")
    return body.replace(old, new, 1)


def getter(body):
    entry = b"L_4D52C0:\n"
    native = b"L_4D52EA:\n    eax = (uint32_t)(TL32((uint32_t)(ecx + 0x20u)));\n"
    # ECX becomes LoadedState before the stock pointer load. Keep the borrowed
    # receiver only in this C invocation and only use it on the confirmed path.
    body = once(body, entry, b"    const uint32_t skyrim_heel_actor = ecx;\n" + entry)
    hook = (b"    if (eax && " + READY + b" &&\n"
            b"        M32(esp) == 0x735095u && M32(esp + 4u) == 0x734FCFu &&\n"
            b"        " + NOTIFY + b"Arm, eax, skyrim_highheel_scene_guest_frame_receipt(c, c->fs_base), SkyrimHighHeelSceneFrame))\n"
            b"        " + NOTIFY + b"Actor, skyrim_heel_actor, eax, 0);\n")
    return once(body, native, native + hook)


def bracket(body, token, before, after):
    call, = [line for line in body.splitlines(keepends=True) if b"M32(esp) = " + token in line]
    return once(body, call, before + call + after)


def frame(body):
    before = (b"    const bool skyrim_heel_frame = " + READY + b" && M32(esp + 8u) == 0x6F3C84u;\n"
              b"    if (skyrim_heel_frame) skyrim_highheel_scene_guest_frame_begin(c, c->fs_base);\n")
    finish = b"    if (skyrim_heel_frame) skyrim_highheel_scene_guest_frame_end(c);\n"
    return bracket(bracket(body, b"0x69477Fu;", before, b""), b"0x69479Du;", b"", finish)


def observe_wait(body):
    # The bookkeeping import overwrites eax on success. Read it immediately
    # after the actual wait, before the original success branch consumes it.
    after = (b"    if (" + READY + b" && M32(esp + 4u) == 0x69477Fu)\n"
             b"        skyrim_highheel_scene_guest_frame_wait(c, c->fs_base, eax);\n")
    return bracket(body, b"0x6A0902u;", b"", after)


def root_update(body):
    native = b"    edx = (uint32_t)(TL32((uint32_t)(eax + 0xA4u)));\n"
    begin = (b"    const bool skyrim_heel_scene = " + ACTIVE + b" && M32(esp + 4u) == 0x734FF1u &&\n"
             b"        " + NOTIFY + b"Root, esi, edx, 0);\n")
    # The original root call owns the receiver. Run only through its downward pass.
    return bracket(once(body, native, native + begin), b"0xAAF336u;", b"",
                   b"    if (skyrim_heel_scene) " + NOTIFY + b"Finish, esi, 0, 0);\n")


def children(body):
    for name, token in ((b"first", b"0xAADA3Fu;"), (b"later", b"0xAADABFu;")):
        flag = b"skyrim_heel_child_" + name
        begin = (b"    const bool " + flag + b" = " + ACTIVE + b" &&\n"
                 b"        " + NOTIFY + b"Enter, esi, edi, edx);\n")
        end = b"    if (" + flag + b") " + NOTIFY + b"Leave, esi, edi, 0);\n"
        body = bracket(body, token, begin, end)
    return body


def receiver(body):
    native = b"    eax = (uint32_t)(TL32((uint32_t)(esi + 0x18u)));\n"
    # The original helper repurposes esi and eax for its copy. Save the
    # receiver and parent only on the reviewed downward path, then observe
    # the receiver's transform immediately after the last original store.
    start = (b"    const bool skyrim_heel_sample = " + ACTIVE + b" &&\n"
             b"        M32(esp + 0x38u) == 0xAB53F8u && M32(esp + 0x3Cu) == 0xAAD9E1u;\n"
             b"    const uint32_t skyrim_heel_node = skyrim_heel_sample ? esi : 0;\n"
             b"    const uint32_t skyrim_heel_parent = skyrim_heel_sample ? eax : 0;\n")
    last_store = b"    MF32((uint32_t)(edx + 0x30u)) = (float)ST(0); fpop(c);\n"
    sample = (b"    if (skyrim_heel_sample) " + NOTIFY +
              b"Member, skyrim_heel_node, skyrim_heel_parent, SkyrimHighHeelScenePose);\n")
    compose = b"L_AB5391:\n"
    before_compose = (b"    if (skyrim_heel_sample) " + NOTIFY +
                      b"PreCompose, skyrim_heel_node, skyrim_heel_parent, 0);\n")
    body = once(body, compose, compose + before_compose)
    return once(once(body, native, native + start), last_store, last_store + sample)


def apply(name, original, base):
    if name not in DIGESTS or sha256(original).hexdigest() != DIGESTS[name]:
        raise ValueError("Scene app unit differs from the reviewed Classic lift")
    for address, edit in {"exe_34.c": ((0x4D52C0, getter),),
                          "exe_106.c": ((0x694770, frame),),
                          "exe_108.c": ((0x6A08F0, observe_wait),),
                          "exe_275.c": ((0xAAF320, root_update), (0xAAD9A0, children)),
                          "exe_276.c": ((0xAB5370, receiver),)}[name]:
        # Prior compatible stages only touch disjoint function bodies.
        native = function(original, address)
        base = once(base, native, edit(native))
    return base if base.startswith(HEADER) else HEADER + base


def apply_transform(original, observed):
    if sha256(original).hexdigest() != DIGESTS["exe_276.c"]:
        raise ValueError("Local transform unit differs from the reviewed Classic lift")
    base = apply("exe_276.c", original, original)
    if observed != base:
        raise ValueError("Local transform requires the reviewed scene observation")
    receiver_body = function(base, 0xAB5370)
    staged = replace_local_z(receiver_body, b"skyrim_heel_sample && skyrim_highheel_transform_guest_selected &&\n"
        b"        skyrim_highheel_transform_guest_local_z(c, esi, eax, M32(edx + 0x2cu), &z)")
    return once(base, receiver_body, staged)


def private_fixture(*units):
    return hosted_fixture(*units, runtime=True)
