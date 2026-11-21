/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "lift_prelude.h"
#include "mod_ece_slot.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    PLAYER = 0x1B2E8E4, ACTOR = 0x20000, BASE = 0x21000, FACE = 0x22000, MENU = 0x23000,
    MORPHS = 0x24000, ROWS = 0x25000, DESCRIPTOR = 0x26000, PART_LIST = 0x27000,
    OVERRIDE_LIST = 0x28000, ACTOR_VTABLE = 0x29000, GET_FACE = 0x2A000, MANAGER = 0x2B000,
    TRI_AREA = 0x30000, ACTOR_STACK = 0x40000, SELECTOR_STACK = 0x41000,
    NOSE_UP = 0x42000, NOSE_DOWN = 0x42020, STOCK_NOSE = 1,
    HEADS = 3, MAX_STEPS = 2 * HEADS, RECORD_BYTES = 36,
};
enum { VALID, STALE_GEOMETRY, MISSING_RECORD, WRONG_VERTEX_COUNT, UNKNOWN_SOURCE };

typedef struct {
    uint32_t part, geometry, witnessed_geometry, tri;
    uint16_t vertices, witnessed_vertices;
    bool sourced;
} Head;

typedef struct {
    uint32_t part, geometry, record, name;
    float weight;
} Step;

uint8_t *MEM;
static CPU owner;
static Head heads[HEADS];
static Step steps[MAX_STEPS];
static unsigned captured, replayed, refusals, up_ordinal, down_ordinal;
static bool dry, alternate;

static uint32_t records(uint32_t tri) { return tri + 0x100; }
static uint32_t record_address(uint32_t tri, unsigned index) { return records(tri) + index * RECORD_BYTES; }
static uint32_t morph_address(void) { return MORPHS + 4 * STOCK_NOSE; }

bool test_dry_run(CPU *c) {
    assert(c == &owner);
    return dry;
}

void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(c == &owner && target == GET_FACE && c->r[1] == ACTOR);
    assert(M32(c->r[4]) == 0x87F70D);
    c->r[0] = FACE;
    c->r[4] += 4;
}

void f_44D240(CPU *c) {
    assert(c->r[1] == ACTOR);
    c->r[0] = BASE;
    c->r[4] += 4;
}

void f_F531CC(CPU *c) {
    fpush(c, fabs(MF64(c->r[4] + 4)));
    c->r[4] += 4;
}

// Interning is backed by the two known test names and stock selector table.
void f_A511C0(CPU *c) {
    const uint32_t name = M32(c->r[4] + 4);
    assert(name == NOSE_UP || name == NOSE_DOWN);
    M32(c->r[1]) = name;
    c->r[4] += 8;
}
void f_A511B0(CPU *c) { c->r[4] += 4; }
void f_A48D60(CPU *c) { (void)c; abort(); }
void f_560150(CPU *c) { (void)c; abort(); }
void f_A4BC20(CPU *c) { (void)c; abort(); }
void f_A4C220(CPU *c) { (void)c; abort(); }

// The real visitor chooses the ordinary or override list and order.
// The override provider, like the backing scene graph, is authored.
void f_5681C0(CPU *c) {
    assert(c->r[1] == BASE);
    c->r[0] = alternate;
    c->r[4] += 4;
}
void f_568280(CPU *c) {
    assert(alternate && M32(c->r[4] + 4) == BASE);
    c->r[0] = OVERRIDE_LIST;
    c->r[4] += 4;
}
void f_5682F0(CPU *c) {
    assert(alternate && M32(c->r[4] + 4) == BASE);
    c->r[0] = HEADS;
    c->r[4] += 4;
}

static uint32_t select_record(const Head *head, uint32_t name_ref) {
    CPU selector = owner;
    selector.r[1] = head->tri;
    selector.r[4] = SELECTOR_STACK;
    M32(SELECTOR_STACK + 4) = SELECTOR_STACK + 0x20;
    M32(SELECTOR_STACK + 8) = name_ref;
    f_5A7570(&selector);
    assert(selector.r[4] == SELECTOR_STACK + 12);
    return selector.r[0];
}

void f_5A4070(CPU *c) {
    // Stand-in for the state-changing FaceGen path at this exact original
    // call edge. Source/geometry receipts are fixture inputs. The real
    // record selector below sees the same name and backing TRI per part.
    const uint32_t sp = c->r[4], part = M32(sp + 8), name_ref = M32(sp + 12);
    const uint32_t name = M32(name_ref);
    assert(c == &owner && c->r[1] == MANAGER && M32(sp) == 0x5A48D2 && M32(sp + 4) == FACE);
    assert(name == NOSE_UP || name == NOSE_DOWN);
    const Head *head = NULL;
    for (unsigned i = 0; i < HEADS; ++i) {
        if (heads[i].part == part) head = &heads[i];
    }
    assert(head);
    const uint32_t record = select_record(head, name_ref);
    const uint32_t expected = record_address(head->tri, name == NOSE_UP ? up_ordinal : down_ordinal);
    const Step actual = {part, head->geometry, record, name, MF32(sp + 16)};
    const bool verified = head->sourced && head->geometry == head->witnessed_geometry &&
                          head->vertices == head->witnessed_vertices && record == expected;
    if (dry) {
        assert(captured < MAX_STEPS);
        steps[captured++] = actual;
        if (!verified) ++refusals;
    } else {
        assert(verified && !refusals && captured == MAX_STEPS && replayed < captured);
        const Step *checked = &steps[replayed++];
        assert(checked->part == actual.part && checked->geometry == actual.geometry &&
               checked->record == actual.record && checked->name == actual.name && checked->weight == actual.weight);
        MF32(actual.geometry + (name == NOSE_DOWN ? 4 : 0)) += actual.weight;
    }
    c->r[4] += 20;
}

static void slider(float value, bool verify) {
    dry = verify;
    owner = (CPU){0};
    owner.r[1] = MENU;
    owner.r[4] = ACTOR_STACK;
    M32(ACTOR_STACK) = 0x885611;
    MF32(ACTOR_STACK + 4) = value;
    M32(ACTOR_STACK + 8) = 0;
    f_87F6E0(&owner);
    assert(owner.r[4] == ACTOR_STACK + 12 && owner.top == 0);
}

static void run(bool positive, bool override, unsigned issue) {
    static const char pos_slot[] = "# CharacterMakingExtender face data\n# character race: Nord\n# version: 2.0\n# 'uniqueID, value' pair\n12, 0.75\n";
    static const char neg_slot[] = "# CharacterMakingExtender face data\n# character race: Nord\n# version: 2.0\n# 'uniqueID, value' pair\n12, -0.75\n";
    const char *slot = positive ? pos_slot : neg_slot;
    const float old = positive ? -0.25f : 0.25f;
    const uint32_t old_name = positive ? NOSE_DOWN : NOSE_UP, new_name = positive ? NOSE_UP : NOSE_DOWN;
    const unsigned last = override ? 0 : HEADS - 1;
    float value = 0;
    assert(skyrim_ece_slot_stock_nose_height(slot, strlen(slot), &value) == SKYRIM_ECE_SLOT_OK);
    assert(value == (positive ? 0.75f : -0.75f));
    alternate = override;
    captured = replayed = refusals = 0;
    MF32(morph_address()) = old;
    MF32(DESCRIPTOR + 0x12C) = old;
    for (unsigned i = 0; i < HEADS; ++i) {
        Head *head = &heads[i];
        M32(record_address(head->tri, up_ordinal)) = NOSE_UP;
        M32(record_address(head->tri, down_ordinal)) = NOSE_DOWN;
        MF32(head->geometry) = positive ? 0.0f : 0.25f;
        MF32(head->geometry + 4) = positive ? 0.25f : 0.0f;
        head->sourced = true;
        head->witnessed_geometry = head->geometry;
        head->witnessed_vertices = head->vertices;
    }
    Head *late = &heads[last];
    if (issue == STALE_GEOMETRY) late->witnessed_geometry += 8;
    if (issue == WRONG_VERTEX_COUNT) ++late->witnessed_vertices;
    if (issue == UNKNOWN_SOURCE) late->sourced = false;
    if (issue == MISSING_RECORD) M32(record_address(late->tri, positive ? up_ordinal : down_ordinal)) = 0;

    uint8_t before[HEADS][8], morphs_before[0x5C];
    for (unsigned i = 0; i < HEADS; ++i) memcpy(before[i], MEM + heads[i].geometry, sizeof before[i]);
    memcpy(morphs_before, MEM + MORPHS, sizeof morphs_before);
    slider(value, true);
    assert(captured == MAX_STEPS && replayed == 0);
    for (unsigned i = 0; i < HEADS; ++i) {
        const unsigned index = override ? HEADS - i - 1 : i;
        assert(steps[i].part == heads[index].part && steps[i].name == old_name);
        assert(steps[HEADS + i].part == heads[index].part && steps[HEADS + i].name == new_name);
        assert(steps[i].weight == -0.25f && steps[HEADS + i].weight == 0.75f);
        assert(!memcmp(before[i], MEM + heads[i].geometry, sizeof before[i]));
    }
    assert(!memcmp(morphs_before, MEM + MORPHS, sizeof morphs_before) && MF32(DESCRIPTOR + 0x12C) == old);

    if (issue != VALID) {
        assert(refusals == (issue == MISSING_RECORD ? 1u : 2u));
        if (issue == MISSING_RECORD) assert(!steps[MAX_STEPS - 1].record);
        return;
    }
    assert(!refusals);
    slider(value, false);
    assert(replayed == MAX_STEPS && MF32(morph_address()) == value && MF32(DESCRIPTOR + 0x12C) == value);
    for (unsigned i = 0; i < HEADS; ++i) {
        const Head *head = &heads[i];
        assert(MF32(head->geometry) == (positive ? 0.75f : 0.0f));
        assert(MF32(head->geometry + 4) == (positive ? 0.0f : 0.75f));
    }
}

int main(int argc, char **argv) {
    assert(argc == 1 || argc == 3);
    up_ordinal = argc == 3 ? (unsigned)atoi(argv[1]) : 3;
    down_ordinal = argc == 3 ? (unsigned)atoi(argv[2]) : 2;
    assert(up_ordinal < 124 && down_ordinal < 124 && up_ordinal != down_ordinal);
    MEM = calloc(1, PLAYER + 4);
    assert(MEM);
    M32(PLAYER) = ACTOR;
    M32(ACTOR) = ACTOR_VTABLE;
    M32(ACTOR_VTABLE + 0x184) = GET_FACE;
    M32(BASE + 0x15C) = MORPHS;
    M32(BASE + 0x14C) = PART_LIST;
    M8(BASE + 0x150) = HEADS;
    M32(MENU + 0xC0) = ROWS;
    M32(ROWS + 4) = DESCRIPTOR;
    M32(DESCRIPTOR + 0x120) = STOCK_NOSE;
    M32(0x12E3538) = MANAGER;
    MF32(0x107A304) = -2.0f;
    MF32(0x1183144) = 0.01f;
    const uint32_t signs = 0x12593A8 + STOCK_NOSE * 12;
    M32(signs) = 2;
    M32(signs + 4) = NOSE_DOWN;
    M32(signs + 8) = NOSE_UP;
    for (unsigned i = 0; i < HEADS; ++i) {
        const uint32_t tri = TRI_AREA + i * 0x2000;
        const uint16_t vertices = argc == 3 && i == 0 ? 996 : 9;
        heads[i] = (Head){0x50000 + i * 0x10, 0x51000 + i * 0x10, 0, tri, vertices, 0, false};
        M32(PART_LIST + i * 4) = heads[i].part;
        M32(OVERRIDE_LIST + (HEADS - i - 1) * 4) = heads[i].part;
        M32(tri + 8) = tri + 0x80;
        M32(tri + 0x80) = records(tri);
        M32(tri + 0x88) = 124;
        M32(tri + 0x40) = 0x80000000;
        M8(tri + 0x44) = (uint8_t)down_ordinal;
        M8(tri + 0x45) = (uint8_t)up_ordinal;
        M32(tri + 0x48) = 2;
    }
    for (unsigned issue = VALID; issue <= UNKNOWN_SOURCE; ++issue) {
        run(true, false, issue);
        run(false, true, issue);
    }
    free(MEM);
    return 0;
}
