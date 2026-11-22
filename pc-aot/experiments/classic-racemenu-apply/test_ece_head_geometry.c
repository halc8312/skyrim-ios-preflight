/* SPDX-License-Identifier: GPL-3.0-only */
#include "ece_head_geometry.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
    FACE = 0x12000, MIDDLE = 0x12200, FIRST = 0x12400, SECOND = 0x12600,
    CHILDREN = 0x13000, SUBTREE = 0x13500, KEY = 0x14000, STACK = 0x15000,
    FACE_TABLE = 0x10bea8c, NODE_TABLE = 0x11171a4, TRI_TABLE = 0x1117e6c,
    STRIPS_TABLE = 0x1117ff4, FLAT_TABLE = 0x11186a4,
    VISIT = 0xaae1d0, LEAF = 0xaaf450, AS_GEOMETRY = 0xc707b0, AS_NULL = 0x5eadd0,
    FACE_NAME = 0x17a10, PART_NAME = 0x17a20, OTHER_NAME = 0x17a30,
    PART_LIST = 0x18000, PART_A = 0x18100, PART_B = 0x18200, PART_SKIP = 0x18300,
    RENDER_NAME = 0x18404, FIRST_PATH = 0x18504, SECOND_PATH = 0x18604,
    DATA_A = 0x18800, DATA_B = 0x18900, TRI_MODEL_TABLE = 0x10a2e08,
    LIMIT = 0x1200000
};

uint8_t *MEM;
void f_AAE1D0(CPU *);
void f_AAF450(CPU *);
void f_C707B0(CPU *);
void f_5EADD0(CPU *);
void f_A50AC0(CPU *);
void f_4F8CA0(CPU *);

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(M32(cpu->r[4]) == 0xaae208);
    if (target == VISIT) f_AAE1D0(cpu);
    else if (target == LEAF) f_AAF450(cpu);
    else abort();
}

static bool read_memory(void *context, uint32_t address, void *out, size_t size) {
    assert(context == MEM && size == 4);
    if (address > LIMIT - size) return false;
    memcpy(out, (const uint8_t *)context + address, size);
    return true;
}

static void object(uint32_t node, uint32_t vtable, uint32_t name, uint32_t parent) {
    M32(node) = vtable;
    M32(node + 8) = name;
    M32(node + 0x18) = parent;
}

static void list(uint32_t parent, uint32_t address, uint16_t count) {
    M32(parent + 0xac) = address;
    M16(parent + 0xb0) = count;
    M16(parent + 0xb2) = count;
}

static uint32_t original(uint32_t name) {
    CPU cpu = {.r = {0, FACE, 0, 0, STACK}};
    M32(KEY) = name;
    M32(STACK + 4) = KEY;
    f_AAE1D0(&cpu);
    const uint32_t matched = cpu.r[0];
    assert(cpu.r[4] == STACK + 8);
    if (!matched) return 0;
    cpu.r[1] = matched;
    uint32_t cast = M32(M32(matched) + 0x20);
    if (cast == AS_GEOMETRY) f_C707B0(&cpu);
    else if (cast == AS_NULL) f_5EADD0(&cpu);
    else abort();
    assert(cpu.r[4] == STACK + 12);
    return cpu.r[0];
}

static void agrees(uint32_t name, uint32_t expected) {
    uint8_t before[KEY - FACE];
    memcpy(before, MEM + FACE, sizeof before);
    uint32_t found = 1;
    assert(ece_head_geometry(read_memory, MEM, FACE, name, &found) && found == expected);
    assert(!memcmp(before, MEM + FACE, sizeof before));
    assert(original(name) == expected);
}

static void refuses(uint32_t name) {
    uint8_t before[KEY - FACE];
    memcpy(before, MEM + FACE, sizeof before);
    uint32_t untouched = 0x12345678;
    assert(!ece_head_geometry(read_memory, MEM, FACE, name, &untouched));
    assert(untouched == 0x12345678 && !memcmp(before, MEM + FACE, sizeof before));
}

static uint32_t original_path(uint32_t part) {
    CPU cpu = {.r = {0, part + 0x20, 0, 0, STACK}};
    f_A50AC0(&cpu);
    assert(cpu.r[4] == STACK + 4);
    if (!cpu.r[0]) return 0;
    cpu.r[1] = part + 0x78;
    f_4F8CA0(&cpu);
    const uint32_t path = cpu.r[0];
    M32(KEY) = path;
    cpu.r[1] = KEY;
    f_A50AC0(&cpu);
    return cpu.r[0] ? path : 0;
}

static void init_part(uint32_t part, uint32_t path, uint32_t node_name) {
    M8(part + 0x12) = 0x0c;
    M32(part + 0x20) = RENDER_NAME;
    M32(part + 0x78) = TRI_MODEL_TABLE;
    M32(part + 0x7c) = path;
    M32(part + 0x94) = node_name;
}

static void test_layout(void) {
    M32(TRI_MODEL_TABLE + 0x10) = 0x4f8ca0;
    M32(RENDER_NAME - 4) = M32(FIRST_PATH - 4) = M32(SECOND_PATH - 4) = 16;
    M32(FIRST + 0xb0) = DATA_A;
    M32(SECOND + 0xb0) = DATA_B;
    M16(DATA_A + 8) = 996;
    M16(DATA_B + 8) = 150;
    M32(SECOND + 8) = 0x17a40;
    init_part(PART_A, FIRST_PATH, PART_NAME);
    init_part(PART_B, SECOND_PATH, 0x17a40);
    init_part(PART_SKIP, 0, PART_NAME);
    M32(PART_LIST) = PART_A;
    M32(PART_LIST + 8) = PART_SKIP;
    M32(PART_LIST + 12) = PART_B;
    EceHeadLayout layout;
    assert(ece_head_layout(read_memory, MEM, FACE, PART_LIST, 4, &layout));
    assert(layout.count == 2 && layout.parts[0].part == PART_A && layout.parts[1].part == PART_B);
    const EceHeadPart first = layout.parts[0], second = layout.parts[1];
    assert(first.geometry == original(M32(PART_A + 0x94)) && first.model_data == DATA_A &&
           first.vertices == 996 && first.tri_path == original_path(PART_A));
    assert(second.geometry == original(M32(PART_B + 0x94)) && second.model_data == DATA_B &&
           second.vertices == 150 && second.tri_path == original_path(PART_B));
    assert(original_path(PART_SKIP) == 0);

    unsigned char previous[sizeof layout];
    memcpy(previous, &layout, sizeof previous);
    M16(DATA_B + 8) = 0;
    assert(!ece_head_layout(read_memory, MEM, FACE, PART_LIST, 4, &layout));
    assert(!memcmp(previous, &layout, sizeof previous));
    M16(DATA_B + 8) = 150;
    M32(PART_B + 0x94) = PART_NAME;
    assert(!ece_head_layout(read_memory, MEM, FACE, PART_LIST, 4, &layout));
    assert(!memcmp(previous, &layout, sizeof previous));
    assert(!ece_head_layout(read_memory, MEM, FACE, PART_LIST, ECE_HEAD_PART_LIMIT + 1, &layout));
    assert(!memcmp(previous, &layout, sizeof previous));
}

int main(void) {
    MEM = calloc(1, LIMIT);
    assert(MEM);
    M32(FACE_TABLE + 0x9c) = M32(NODE_TABLE + 0x9c) = VISIT;
    M32(FACE_TABLE + 0x20) = M32(NODE_TABLE + 0x20) = AS_NULL;
    M32(TRI_TABLE + 0x9c) = M32(STRIPS_TABLE + 0x9c) = LEAF;
    M32(TRI_TABLE + 0x20) = M32(STRIPS_TABLE + 0x20) = AS_GEOMETRY;
    M32(FLAT_TABLE + 0x9c) = 0xabe390;
    object(FACE, FACE_TABLE, FACE_NAME, 0);
    object(MIDDLE, NODE_TABLE, OTHER_NAME, FACE);
    object(FIRST, TRI_TABLE, PART_NAME, MIDDLE);
    object(SECOND, STRIPS_TABLE, PART_NAME, FACE);
    list(FACE, CHILDREN, 3);
    list(MIDDLE, SUBTREE, 3);
    M32(CHILDREN + 4) = MIDDLE;
    M32(CHILDREN + 8) = SECOND;
    M32(SUBTREE + 8) = FIRST;
    agrees(PART_NAME, FIRST);                 /* Depth-first, despite a second matching part. */
    agrees(FACE_NAME, 0);                     /* Name matched but cast returned null. */
    agrees(OTHER_NAME, 0);
    agrees(0x17a40, 0);                       /* Whole visited tree missed. */
    M32(MIDDLE + 8) = PART_NAME;
    agrees(PART_NAME, 0);                    /* Don't continue after a non-geometry name match. */
    M32(MIDDLE + 8) = OTHER_NAME;
    M32(SUBTREE + 8) = 0;
    agrees(PART_NAME, SECOND);
    M32(MIDDLE) = FLAT_TABLE;
    refuses(PART_NAME);                     /* A flattened lookup could allocate and attach. */
    M32(MIDDLE) = NODE_TABLE;
    M32(SUBTREE + 8) = FIRST;
    M32(FIRST + 0x18) = FACE;
    refuses(PART_NAME);                     /* Reparented subtree differs from the selected edge. */
    M32(FIRST + 0x18) = MIDDLE;
    M16(MIDDLE + 0xb0) = 2;
    refuses(PART_NAME);                     /* Span beyond the capacity. */
    list(MIDDLE, SUBTREE, 257);
    refuses(PART_NAME);                     /* Work bound includes holes. */
    list(MIDDLE, 0xfffffffc, 1);
    refuses(PART_NAME);                     /* Unreadable array leaves no result. */
    list(MIDDLE, SUBTREE, 3);
    test_layout();
    free(MEM);
}
