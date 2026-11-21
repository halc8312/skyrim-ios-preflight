/* SPDX-License-Identifier: GPL-3.0-only */
#include "racemenu_apply.h"

#include <assert.h>
#include <math.h>
#include <stdatomic.h>

enum { FRAME_STACK = 0x100, SLIDERS = 0x200, SLIDER_OFFSET = 0x134,
       ACTOR = 0x500, NPC = 0x600, MENU = 0x700, MORPHS = 0x900,
       SETTER_STACK = 0xa00, MARK_STACK = 0xb00 };

typedef struct { int unused; } CPU;
static CPU cpu;
#include "commit-state.inc"
static unsigned reads, calls, acknowledgements;
static uint32_t return_address = 0x8859B4u, label = 0x800, descriptor_label = 0x800;
static uint32_t kind = 2, morph = 1, actor = ACTOR, current_base = NPC;
static float minimum = -1, maximum = 1, argument = -0.25f, desired = 0.75f;
static bool pending = true;
static bool setter_writes = true, valid_marker = true, mark_accepted = true;
static uint32_t payload;

static uint32_t word(uint32_t address) {
    ++reads;
    switch (address) {
    case 0x1B18B94u: return label;
    case 0x1B2E8E4u: return actor;
    case SLIDERS + SLIDER_OFFSET + 0x18: return descriptor_label;
    case SLIDERS + SLIDER_OFFSET + 0x14: return kind;
    case SLIDERS + SLIDER_OFFSET + 0x120: return morph;
    case ACTOR + 0x24: return current_base;
    case FRAME_STACK: return return_address;
    case FRAME_STACK + 0x37C: return return_address;
    case SETTER_STACK: return 0x885611u;
    case SETTER_STACK + 8: return 1;
    case SETTER_STACK + 12: return ((union {float number; uint32_t bits;}){.number = argument}).bits;
    case NPC + 0x15c: return MORPHS;
    case MORPHS + 4: return payload;
    case MARK_STACK: return 0x8859BBu;
    default: assert(!"unexpected word offset"); return 0;
    }
}

static float *floating(uint32_t address) {
    ++reads;
    switch (address) {
    case SLIDERS + SLIDER_OFFSET: return &minimum;
    case SLIDERS + SLIDER_OFFSET + 4: return &maximum;
    case FRAME_STACK:
    case FRAME_STACK + 0x28: return &argument;
    default: assert(!"unexpected float offset"); return &argument;
    }
}

#define M32(address) word(address)
#define TL32(address) word(address)
#define MF32(address) (*floating(address))

static bool skyrim_racemenu_probe_pending(void) { return pending; }

static bool skyrim_racemenu_probe_nose_height(uint32_t menu, uint32_t live_actor, uint32_t base,
                                             float min, float max, float *value) {
    assert(menu == MENU && live_actor == ACTOR && base == NPC);
    assert(min == minimum && max == maximum);
    ++calls;
    *value = desired;
    pending = false;
    return true;
}

static void skyrim_racemenu_probe_did_apply(void) { ++acknowledgements; }

static void stock_setter(CPU *c) {
    uint32_t esp = SETTER_STACK, ecx = NPC;
#include "setter-admit.inc"
    esp += 8;
#include "setter-before.inc"
    if (setter_writes) payload = ((union {float number; uint32_t bits;}){.number = argument}).bits;
#include "setter-after.inc"
}

static void stock_mark(CPU *c) {
    const uint32_t esp = MARK_STACK, ecx = NPC, edx = 0x10A5964u;
    uint32_t eax = valid_marker ? 0x55B970u : 0x55b971u;
#include "mark-admit.inc"
#include "mark-before.inc"
    eax = mark_accepted;
#include "mark-after.inc"
}

static void preview_then_apply(uint32_t skyrim_racemenu_preview_base, bool actor_changes) {
    CPU *c = &cpu;
    uint32_t skyrim_racemenu_accepted_actor = 0;
    float skyrim_racemenu_original_nose = 0, skyrim_racemenu_selected_nose = 0;
    const uint32_t esp = FRAME_STACK, edx = SLIDERS, esi = SLIDER_OFFSET, ebp = MENU;
#include "clear-commit.inc"
#include "substitute.inc"
    if (actor_changes) current_base = NPC + 1;
#include "before.inc"
    stock_setter(c);
    stock_mark(c);
}

static void preview(uint32_t base) { preview_then_apply(base, false); }

int main(void) {
    pending = false;
    preview(NPC);
    assert(!reads && !calls && argument == -0.25f);
    pending = true;
    return_address = 0x885DD9u;
    preview(NPC);
    assert(!calls && argument == -0.25f);
    return_address = 0x8859B4u;
    descriptor_label = 0x801;
    preview(NPC);
    descriptor_label = label;
    kind = 3;
    preview(NPC);
    kind = 2;
    morph = 0;
    preview(NPC);
    morph = 1;
    actor = 0;
    preview(NPC);
    actor = ACTOR;
    current_base = 0x601;
    preview(NPC);
    current_base = NPC;
    preview(0);
    assert(!calls && argument == -0.25f);
    desired = 2;
    preview(NPC);
    assert(calls == 1 && argument == -0.25f);
    pending = true;
    desired = NAN;
    preview(NPC);
    assert(calls == 2 && argument == -0.25f);
    pending = true;
    desired = 0.75f;
    preview(NPC);
    preview(NPC);
    assert(calls == 3 && acknowledgements == 1 && argument == 0.75f);
    pending = true;
    argument = -0.25f;
    preview_then_apply(NPC, true);
    assert(calls == 4 && acknowledgements == 1 && argument == -0.25f);
    current_base = NPC;
    pending = true;
    setter_writes = false;
    preview(NPC);
    assert(calls == 5 && acknowledgements == 1);
    pending = setter_writes = true;
    valid_marker = false;
    preview(NPC);
    assert(calls == 6 && acknowledgements == 1);
    pending = valid_marker = true;
    mark_accepted = false;
    preview(NPC);
    assert(calls == 7 && acknowledgements == 1);
    return 0;
}
