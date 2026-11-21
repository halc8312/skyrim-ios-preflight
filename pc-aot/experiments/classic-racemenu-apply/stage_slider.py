"""Stage an optional Nose Height substitution in the owned Classic lift."""
from __future__ import annotations

import hashlib

SLIDER_UNIT = "exe_185.c"
SLIDER_SHA256 = "4b53b0aa8cf4974dbfd01654ad1e519d4d053b3b933e414c9d6decad9ce32a10"
UPDATE_UNIT = "exe_186.c"
UPDATE_SHA256 = "bf380ee42a2aaed3e1614e03051c99cc66cfea8eb122fa0eb2b3b6da5c271add"
ENTRY = b"L_882290:\n"
PREVIEW = b"L_88231D:\n"
# At this point edx points at the live array of the active race and sex.
DESCRIPTOR = (b"    edx = (uint32_t)(TL32((uint32_t)(ecx + eax + 0x4u)));\n"
              b"    eax = (uint32_t)(TL32((uint32_t)(edx + esi + 0x18u)));\n"
              b"    M32((uint32_t)(esp + 0xBCu)) = (uint32_t)(eax);\n")
OPEN = b"    esp -= 4; M32(esp) = 0x8859B4u; SPILL(); f_882290(c); RELOAD();\n"
APPLY = b"    esp -= 4; M32(esp) = 0x885611u; SPILL(); f_87F6E0(c); RELOAD();\n"
DECLARATIONS = (b"#include <math.h>\n#include <stdbool.h>\n"
                b"bool skyrim_racemenu_probe_pending(void);\n"
                b"bool skyrim_racemenu_probe_nose_height(uint32_t menu, uint32_t actor, uint32_t base,\n"
                b"                                     float min, float max, float *value);\n")
SUBSTITUTE = b"""    if (skyrim_racemenu_probe_pending() && M32(esp + 0x37Cu) == 0x8859B4u) {
        // The stock creator initializes these fields for sRSMNoseHeight.
        const uint32_t descriptor = edx + esi;
        const uint32_t label = TL32(0x1B18B94u);
        if (label && TL32(descriptor + 0x18u) == label &&
            TL32(descriptor + 0x14u) == 2u && TL32(descriptor + 0x120u) == 1u) {
            const uint32_t actor = TL32(0x1B2E8E4u);
            if (actor && skyrim_racemenu_preview_base &&
                TL32(actor + 0x24u) == skyrim_racemenu_preview_base) {
                const float min = MF32(descriptor);
                const float max = MF32(descriptor + 4u);
                float chosen = 0.0f;
                if (skyrim_racemenu_probe_nose_height(ebp, actor, skyrim_racemenu_preview_base,
                                                      min, max, &chosen) &&
                    isfinite(min) && isfinite(max) && min < max &&
                    isfinite(chosen) && min <= chosen && chosen <= max)
                    MF32(esp + 0x28u) = chosen;
            }
        }
    }
"""


def function_span(data: bytes, address: str) -> tuple[int, int]:
    marker = f"\nvoid f_{address}(CPU *c) {{\n".encode("ascii")
    if data.count(marker) != 1:
        raise ValueError(f"Expected exactly one lifted function {address}.")
    start = data.index(marker)
    return start, data.index(b"\n}\n", start) + 3


def stage_slider(slider: bytes, update: bytes) -> bytes:
    if (hashlib.sha256(slider).hexdigest() != SLIDER_SHA256
            or hashlib.sha256(update).hexdigest() != UPDATE_SHA256):
        raise ValueError("Input differs from the reviewed Classic lift.")
    start, end = function_span(update, "885980")
    if update[start:end].count(OPEN) != 1:
        raise ValueError("Expected one stock menu update call.")
    start, end = function_span(slider, "882290")
    body = slider[start:end]
    if any(body.count(anchor) != 1 for anchor in (ENTRY, PREVIEW, DESCRIPTOR, APPLY)):
        raise ValueError("Stock slider construction or application is missing or ambiguous.")
    if not body.index(ENTRY) < body.index(PREVIEW) < body.index(DESCRIPTOR) < body.index(APPLY):
        raise ValueError("Stock slider sites are out of order.")
    prelude_end = slider.index(b'\n#include "lift_prelude.h"\n') + len(b'\n#include "lift_prelude.h"\n')
    body = body.replace(ENTRY, b"    uint32_t skyrim_racemenu_preview_base = 0;\n" + ENTRY)
    body = body.replace(PREVIEW, PREVIEW + b"    skyrim_racemenu_preview_base = eax;\n")
    descriptor_load, name_load = DESCRIPTOR.split(b"\n", 1)
    body = body.replace(DESCRIPTOR, descriptor_load + b"\n" + SUBSTITUTE + name_load)
    return slider[:prelude_end] + DECLARATIONS + slider[prelude_end:start] + body + slider[end:]
