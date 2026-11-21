"""Hash-bound, read-only staging for the private Classic ECE nose witness."""
import hashlib

from stage_slider import DESCRIPTOR, OPEN, APPLY, PREVIEW, function_span

# Each entry is an original-body compilation unit from the owner generation.
DIGESTS = {
    "exe_66.c": "e5aef726544cfa9c4bacf1661bae164b8cd86b6c7e36d2190b201ffd8a8ccd5a",
    "exe_67.c": "35d39d44e2c52e54108f5431ee35b497a72c3ea6b72eafe7a73c6971591bdec6",
    "exe_68.c": "c7f5ef78c7cf90565f0860c29d26bd983a410fc3c078df8a7c59b8949f84577a",
    "exe_185.c": "4b53b0aa8cf4974dbfd01654ad1e519d4d053b3b933e414c9d6decad9ce32a10",
    "exe_186.c": "bf380ee42a2aaed3e1614e03051c99cc66cfea8eb122fa0eb2b3b6da5c271add",
}
DECLARATION = b'#include "ece_nose_witness.h"\n'


def call(site, target):
    return f"    esp -= 4; M32(esp) = 0x{site}u; SPILL(); f_{target}(c); RELOAD();\n".encode()


# Split the descriptor load at the point where the original code has both
# the live sex/race row and the selected descriptor in registers.
DESCRIPTOR_LOAD = DESCRIPTOR.splitlines(keepends=True)[0]
NATIVE_SITE = b"    { uint32_t t_ = edx; esp -= 4; M32(esp) = 0x87F70Du;"
PATH_SITE = b"    { uint32_t t_ = eax; esp -= 4; M32(esp) = 0x5A5BB7u;"
LOOKUP = call("5A5BBD", "5A2B40") + b"    esp = (uint32_t)(add32(esp, (uint32_t)0xCu, &f));\n"
GEOMETRY = b"    { uint32_t t_ = eax; esp -= 4; M32(esp) = 0x5A40BEu;"


def insert(function, anchor, before=b"", after=b""):
    if function.count(anchor) != 1:
        raise ValueError("A reviewed ECE nose witness anchor is missing or ambiguous")
    return function.replace(anchor, before + anchor + after, 1)


def indirect(function, anchor, before=b"", after=b""):
    start = function.index(anchor)
    end = function.index(b"\n", start) + 1
    return insert(function, function[start:end], before, after)


def menu(function):
    function = insert(function, b"L_885980:\n",
                      before=b"    const uint32_t ece_dispatch = M32(esp);\n")
    return insert(function, OPEN, b"    const bool ece_menu = ece_nose_menu(ece_dispatch, esi);\n",
                  b"    ece_nose_menu_end(ece_menu);\n")


def slider(function):
    function = insert(function, b"L_882290:\n", before=b"    uint32_t ece_preview = 0, ece_race = 0;\n")
    function = insert(function, PREVIEW, after=b"    ece_preview = eax;\n")
    descriptor = DESCRIPTOR_LOAD + b"""    ece_race = 0;
    if (M32(esp + 0x37Cu) == 0x8859B4u) {
        const uint32_t descriptor = edx + esi;
        const uint32_t label = TL32(0x1B18B94u);
        if (label && TL32(descriptor + 0x18u) == label &&
            TL32(descriptor + 0x14u) == 2u && TL32(descriptor + 0x120u) == 1u)
            ece_race = TL32(ecx + eax);
    }
""" + DESCRIPTOR[len(DESCRIPTOR_LOAD):]
    if function.count(DESCRIPTOR) != 1:
        raise ValueError("The reviewed ECE descriptor is missing or ambiguous")
    function = function.replace(DESCRIPTOR, descriptor, 1)
    return insert(function, APPLY, b"    const bool ece_nose = ece_nose_begin(ebp, ece_preview, ece_race);\n",
                  b"    ece_nose_end(ece_nose);\n")


def face(function):
    function = insert(function, b"L_87F6E0:\n", before=b"    const uint32_t ece_caller = M32(esp);\n"
                      b"    uint32_t ece_actor;\n")
    return indirect(function, NATIVE_SITE,
                    b"    ece_actor = ecx;\n",
                    b"    ece_nose_face(ece_caller, esi, ece_actor, M32(esp + 0x28u), eax);\n")


def head(function):
    function = insert(function, b"L_5A4070:\n",
                      before=b"    const uint32_t ece_caller = M32(esp), ece_face = M32(esp + 4u),\n"
                      b"                   ece_part = M32(esp + 8u);\n"
                      b"    uint32_t ece_geometry = 0;\n")
    function = indirect(function, GEOMETRY, after=b"    ece_geometry = eax;\n")
    return insert(function, call("5A4133", "5A5B70"),
                  b"    const bool ece_head = ece_nose_head(ece_caller, ece_face, ece_part, ece_geometry);\n",
                  b"    ece_nose_head_end(ece_head);\n")


def lookup(function):
    function = insert(function, b"L_5A5B70:\n", before=b"    const uint32_t ece_caller = M32(esp);\n"
                      b"    bool ece_lookup = false;\n")
    function = indirect(function, PATH_SITE, after=b"    ece_lookup = ece_caller == 0x5A4133u && ece_nose_lookup(eax);\n")
    return insert(function, LOOKUP, after=b"    ece_nose_lookup_end(ece_lookup, eax == 0 ? M32(esp + 0xCu) : 0);\n")


def parse(function):
    function = insert(function, b"L_5A0710:\n", before=b"    bool ece_fresh = false;\n")
    function = insert(function, call("5A078B", "59EF90"),
                      b"    const uint64_t ece_receipt = ece_nose_parse(c, edi, M32(esp + 4u));\n",
                      b"    ece_fresh = ece_nose_parsed(c, ece_receipt, (uint8_t)eax != 0);\n")
    return insert(function, call("5A07D3", "5A0550"), after=b"    ece_nose_constructed(ece_fresh, eax);\n")


def retiring(function):
    return insert(function, b"L_5A0660:\n", after=b"    ece_nose_retiring(ecx);\n")


def morph(function):
    # f_5A75F0: arg1 is BSFixedString*, arg2 is the geometry. The original
    # f_5A7570 compares interned names and returns the selected record.
    function = insert(function, b"L_5A75F0:\n", before=b"    const uint32_t ece_caller = M32(esp);\n"
                      b"    const uint32_t ece_name = M32(esp + 4u);\n"
                      b"    const bool ece_morph = ece_caller == 0x5A5C79u && ece_nose_morph(ecx, M32(esp + 8u));\n")
    return insert(function, call("5A7613", "5A7570"),
                  after=b"    ece_nose_morph_record(ece_morph, ece_morph && ece_name ? TL32(ece_name) : 0, eax != 0);\n")


TRANSFORMS = {
    "exe_66.c": (("5A0710", parse), ("5A0660", retiring)),
    "exe_67.c": (("5A4070", head), ("5A5B70", lookup)),
    "exe_68.c": (("5A75F0", morph),),
    "exe_185.c": (("882290", slider), ("87F6E0", face)),
    "exe_186.c": (("885980", menu),),
}


def stage(unit, data):
    if unit not in DIGESTS or hashlib.sha256(data).hexdigest() != DIGESTS[unit]:
        raise ValueError("ECE nose witness requires the reviewed original Classic lift")
    for address, transform in TRANSFORMS[unit]:
        start, end = function_span(data, address)
        data = data[:start] + transform(data[start:end]) + data[end:]
    return DECLARATION + data
