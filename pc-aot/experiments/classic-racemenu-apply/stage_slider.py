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
SETTER_ENTRY = b"L_87F6E0:\n"
SETTER = b"    esp -= 4; M32(esp) = 0x87FA0Au; SPILL(); f_561180(c); RELOAD();\n"
MARK_ENTRY = b"L_87EDF0:\n"
MARK = (b"    { uint32_t t_ = eax; esp -= 4; M32(esp) = 0x87EE0Bu; SPILL(); { static const FnEntry *ic_; "
        b"const FnEntry *e_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED); if (e_ && e_->addr == t_) e_->fn(c); "
        b"else rt_call_ic(c, t_, &ic_); } RELOAD(); }\n")
COMMIT_STATE = b"""
/* The host admits one preset per launch. Only the admitting CPU writes
 * or clears that request. Other CPUs only inspect the published owner. */
static struct {
    _Atomic(const CPU *) owner;
    uint32_t npc, bits;
    bool written;
} skyrim_racemenu_nose_commit;
"""
DECLARATIONS = (b"#include <math.h>\n#include <stdatomic.h>\n#include <stdbool.h>\n"
                b"bool skyrim_racemenu_probe_pending(void);\n"
                b"bool skyrim_racemenu_probe_nose_height(uint32_t menu, uint32_t actor, uint32_t base,\n"
                b"                                     float min, float max, float *value);\n"
                b"void skyrim_racemenu_probe_did_apply(void);\n" + COMMIT_STATE)
LOCALS = (b"    uint32_t skyrim_racemenu_preview_base = 0, skyrim_racemenu_accepted_actor = 0;\n"
          b"    float skyrim_racemenu_original_nose = 0.0f, skyrim_racemenu_selected_nose = 0.0f;\n")
CLEAR_COMMIT = b"""    if (atomic_load_explicit(&skyrim_racemenu_nose_commit.owner, memory_order_relaxed) == c &&
        M32(esp) == 0x8859B4u)
        atomic_store_explicit(&skyrim_racemenu_nose_commit.owner, NULL, memory_order_relaxed);
"""
SUBSTITUTE = b"""    skyrim_racemenu_accepted_actor = 0;
    if (skyrim_racemenu_probe_pending() && M32(esp + 0x37Cu) == 0x8859B4u) {
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
                    isfinite(chosen) && min <= chosen && chosen <= max) {
                    skyrim_racemenu_original_nose = MF32(esp + 0x28u);
                    skyrim_racemenu_selected_nose = chosen;
                    skyrim_racemenu_accepted_actor = actor;
                    MF32(esp + 0x28u) = chosen;
                }
            }
        }
    }
"""

BEFORE_APPLY = b"""    if (skyrim_racemenu_accepted_actor &&
        (TL32(0x1B2E8E4u) != skyrim_racemenu_accepted_actor ||
         TL32(skyrim_racemenu_accepted_actor + 0x24u) != skyrim_racemenu_preview_base)) {
        MF32(esp) = skyrim_racemenu_original_nose;
        skyrim_racemenu_accepted_actor = 0;
    }
    if (skyrim_racemenu_accepted_actor) {
        const union { float value; uint32_t bits; } chosen = {.value = skyrim_racemenu_selected_nose};
        skyrim_racemenu_nose_commit.npc = skyrim_racemenu_preview_base;
        skyrim_racemenu_nose_commit.bits = chosen.bits;
        skyrim_racemenu_nose_commit.written = false;
        atomic_store_explicit(&skyrim_racemenu_nose_commit.owner, c, memory_order_release);
    }
"""
SETTER_ADMIT = b"""    const bool skyrim_observe_setter =
        atomic_load_explicit(&skyrim_racemenu_nose_commit.owner, memory_order_acquire) == c && M32(esp) == 0x885611u;
"""
SETTER_BEFORE = b"""    const uint32_t skyrim_nose_npc =
        skyrim_observe_setter && ecx == skyrim_racemenu_nose_commit.npc && M32(esp) == 1u &&
        M32(esp + 4u) == skyrim_racemenu_nose_commit.bits ? ecx : 0;
"""
SETTER_AFTER = b"""    if (skyrim_nose_npc) {
        const uint32_t morphs = TL32(skyrim_nose_npc + 0x15cu);
        skyrim_racemenu_nose_commit.written = morphs && morphs != 0x1B11128u &&
            TL32(morphs + 4u) == skyrim_racemenu_nose_commit.bits;
    }
"""
MARK_ADMIT = b"""    const bool skyrim_observe_mark =
        atomic_load_explicit(&skyrim_racemenu_nose_commit.owner, memory_order_acquire) == c && M32(esp) == 0x8859BBu;
"""
MARK_BEFORE = b"""    const bool skyrim_accept_mark = skyrim_observe_mark && skyrim_racemenu_nose_commit.written &&
        ecx == skyrim_racemenu_nose_commit.npc && edx == 0x10A5964u && eax == 0x55B970u;
    if (skyrim_observe_mark) atomic_store_explicit(&skyrim_racemenu_nose_commit.owner, NULL, memory_order_relaxed);
"""
MARK_AFTER = b"""    if (skyrim_accept_mark && (uint8_t)eax) skyrim_racemenu_probe_did_apply();
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
    body = body.replace(ENTRY, LOCALS + ENTRY + CLEAR_COMMIT)
    body = body.replace(PREVIEW, PREVIEW + b"    skyrim_racemenu_preview_base = eax;\n")
    descriptor_load, name_load = DESCRIPTOR.split(b"\n", 1)
    body = body.replace(DESCRIPTOR, descriptor_load + b"\n" + SUBSTITUTE + name_load)
    body = body.replace(APPLY, BEFORE_APPLY + APPLY)
    staged = slider[:prelude_end] + DECLARATIONS + slider[prelude_end:start] + body + slider[end:]
    for name, entry, call, admit, before, after in (
        ("87F6E0", SETTER_ENTRY, SETTER, SETTER_ADMIT, SETTER_BEFORE, SETTER_AFTER),
        ("87EDF0", MARK_ENTRY, MARK, MARK_ADMIT, MARK_BEFORE, MARK_AFTER),
    ):
        start, end = function_span(staged, name)
        body = staged[start:end]
        if body.count(entry) != 1 or body.count(call) != 1 or body.index(call) <= body.index(entry):
            raise ValueError(f"Stock slider confirmation site changed in {name}.")
        body = body.replace(entry, admit + entry).replace(call, before + call + after)
        staged = staged[:start] + body + staged[end:]
    return staged
