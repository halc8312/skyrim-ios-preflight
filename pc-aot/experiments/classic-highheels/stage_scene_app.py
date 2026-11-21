"""Add passive HighHeels diagnostics to the pinned stock scene calls."""

from hashlib import sha256

from stage_effect_magnitude import function
from stage_scene_witness import DEPENDENCIES, UNIT, UNITS, private_fixture as hosted_fixture


DIGESTS = {name: digest for name, digest, _ in UNITS if name in ("exe_34.c", "exe_275.c", "exe_276.c")}
HEADER = b'#include "PcGuestModHooksABI.h"\n'
STATE = b"__atomic_load_n(&skyrim_highheel_scene_guest_state, __ATOMIC_RELAXED)"
ACTIVE = STATE + b" == SkyrimHighHeelSceneActive"
NOTIFY = b"skyrim_highheel_scene_guest_notify(c, SkyrimHighHeelScene"


def once(body, old, new):
    if body.count(old) != 1:
        raise ValueError("Expected one reviewed HighHeels scene call or field load")
    return body.replace(old, new, 1)


def getter(body):
    native = b"L_4D52EA:\n    eax = (uint32_t)(TL32((uint32_t)(ecx + 0x20u)));\n"
    hook = (b"    if (eax && " + STATE + b" == SkyrimHighHeelSceneReady &&\n"
            b"        M32(esp) == 0x735095u && M32(esp + 4u) == 0x734FCFu)\n"
            b"        " + NOTIFY + b"Arm, eax, 0, 0);\n")
    return once(body, native, native + hook)


def bracket(body, token, before, after):
    call, = [line for line in body.splitlines(keepends=True) if b"M32(esp) = " + token in line]
    return once(body, call, before + call + after)


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
    # Read after the stock parent pointer load, inside the node's own update.
    hook = (b"    if (" + ACTIVE + b" &&\n"
            b"        M32(esp + 0x38u) == 0xAB53F8u && M32(esp + 0x3Cu) == 0xAAD9E1u)\n"
            b"        " + NOTIFY + b"Member, esi, eax, 0);\n")
    return once(body, native, native + hook)


def apply(name, original, base):
    if name not in DIGESTS or sha256(original).hexdigest() != DIGESTS[name]:
        raise ValueError("Scene app unit differs from the reviewed Classic lift")
    for address, edit in {"exe_34.c": ((0x4D52C0, getter),),
                          "exe_275.c": ((0xAAF320, root_update), (0xAAD9A0, children)),
                          "exe_276.c": ((0xAB5370, receiver),)}[name]:
        # Prior compatible stages only touch disjoint function bodies.
        native = function(original, address)
        base = once(base, native, edit(native))
    return base if base.startswith(HEADER) else HEADER + base


def private_fixture(*units):
    return hosted_fixture(*units, runtime=True)
