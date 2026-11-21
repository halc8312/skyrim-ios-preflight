/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
    SOURCE = 0x10000, TARGET = 0x11000, WRITER = 0x12000, READER = 0x13000,
    SOURCE_MORPHS = 0x14000, TARGET_MORPHS = 0x15000, STACK = 0x16000,
    DEFAULT_MORPHS = 0x1B11128, MORPH_OFFSET = 0x15C, NOSE = 1, FACE_CHANGED = 0x800,
    OPTIONS = 19, PRESETS = 4, MORPH_BYTES = (OPTIONS + PRESETS) * 4,
    WIRE_BYTES = 1 + 4 + OPTIONS * 4 + 4 + PRESETS * 4
};

uint8_t *MEM;
static uint8_t stream[WIRE_BYTES];
static size_t written, consumed;
static uint32_t allocation;

void test_save_morphs(CPU *c);
void test_load_morphs(CPU *c);

// These callbacks transfer bytes only. The guest selects the count, widths,
// addresses, order and presence marker.
void f_686E30(CPU *c) {
    const uint32_t sp = c->r[4], source = M32(sp + 4), size = M32(sp + 8);
    assert(c->r[1] == WRITER && (size == 1 || size == 4));
    assert(M32(sp + 12) == size && M32(sp + 16) == 0 && size <= sizeof stream - written);
    memcpy(stream + written, MEM + source, size);
    written += size;
    c->r[4] += 20;
}

void f_685E20(CPU *c) {
    const uint32_t sp = c->r[4], target = M32(sp + 4), size = M32(sp + 8);
    assert(c->r[1] == READER && (size == 1 || size == 4));
    assert(M32(sp + 12) == size && size <= written - consumed);
    memcpy(MEM + target, stream + consumed, size);
    consumed += size;
    c->r[4] += 16;
}

void f_A48D60(CPU *c) {
    const uint32_t sp = c->r[4];
    assert(allocation && c->r[1] == 0x1B418B0 && M32(sp + 4) == MORPH_BYTES);
    assert(M32(sp + 8) == 0 && M32(sp + 12) == 0);
    c->r[0] = allocation;
    allocation = 0;
    c->r[4] += 16;
}

static void write_morphs(uint32_t flag) {
    CPU c = {0};
    M32(WRITER + 0x13) = flag;
    c.r[4] = STACK;
    c.r[6] = WRITER;
    c.r[7] = SOURCE;
    written = 0;
    test_save_morphs(&c);
    assert(c.r[4] == STACK);
}

static void read_morphs(uint32_t flag) {
    CPU c = {0};
    M32(READER + 0x28) = flag;
    c.r[4] = STACK;
    c.r[6] = TARGET;
    c.r[7] = READER;
    consumed = 0;
    test_load_morphs(&c);
    assert(c.r[4] == STACK);
}

static void store_nose(uint32_t bits) {
    CPU c = {0};
    M32(SOURCE + MORPH_OFFSET) = DEFAULT_MORPHS;
    allocation = SOURCE_MORPHS;
    c.r[1] = SOURCE;
    c.r[4] = STACK;
    M32(STACK + 4) = NOSE;
    M32(STACK + 8) = bits;
    f_561180(&c);
    assert(!allocation && c.r[4] == STACK + 12);
    assert(M32(SOURCE + MORPH_OFFSET) == SOURCE_MORPHS && M32(SOURCE_MORPHS + NOSE * 4) == bits);
}

static void check_roundtrip(uint32_t bits) {
    store_nose(bits);
    M32(SOURCE_MORPHS + (OPTIONS + PRESETS - 1) * 4) = 7;
    write_morphs(FACE_CHANGED);
    assert(written == WIRE_BYTES && stream[0] == 1 && stream[1] == OPTIONS && stream[1 + 4 + OPTIONS * 4] == PRESETS);
    for (unsigned i = 0; i < 4; ++i)
        assert(stream[1 + 4 + NOSE * 4 + i] == (uint8_t)(bits >> (8 * i)));

    memset(MEM + SOURCE_MORPHS, 0, MORPH_BYTES);
    M32(TARGET + MORPH_OFFSET) = DEFAULT_MORPHS;
    read_morphs(0x400);
    assert(!consumed && M32(TARGET + MORPH_OFFSET) == DEFAULT_MORPHS);

    memset(MEM + TARGET_MORPHS, 0xA5, MORPH_BYTES);
    allocation = TARGET_MORPHS;
    read_morphs(FACE_CHANGED);
    assert(!allocation && consumed == written && M32(TARGET + MORPH_OFFSET) == TARGET_MORPHS);
    assert(M32(TARGET_MORPHS + NOSE * 4) == bits);
    assert(M32(TARGET_MORPHS) == 0x7F7FFFFF && M32(TARGET_MORPHS + (NOSE + 1) * 4) == 0x7F7FFFFF);
    assert(M32(TARGET_MORPHS + OPTIONS * 4) == UINT32_MAX);
    assert(M32(TARGET_MORPHS + (OPTIONS + PRESETS - 1) * 4) == 7);
}

int main(void) {
    MEM = calloc(1, STACK + 0x200);
    assert(MEM);
    check_roundtrip(0x3EAAAAAB);
    check_roundtrip(0xBF100001);
    check_roundtrip(0x80000000); // Signed zero also survives byte-for-byte.

    write_morphs(0x400);
    assert(!written);
    for (unsigned i = 0; i < 2; ++i) {
        M32(SOURCE + MORPH_OFFSET) = i == 0 ? DEFAULT_MORPHS : 0;
        write_morphs(FACE_CHANGED);
        assert(written == 1 && stream[0] == 0);
        M32(TARGET + MORPH_OFFSET) = DEFAULT_MORPHS;
        read_morphs(FACE_CHANGED);
        assert(consumed == 1 && M32(TARGET + MORPH_OFFSET) == DEFAULT_MORPHS);
    }

    free(MEM);
    return 0;
}
