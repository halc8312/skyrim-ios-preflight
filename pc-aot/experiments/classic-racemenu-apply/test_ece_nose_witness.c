/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "lift_prelude.h"
#include "ece_nose_witness.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
    PLAYER = 0x1B2E8E4, ACTOR = 0x20000, BASE = 0x22000, RACE = 0x24000, MENU = 0x26000,
    ROWS = 0x28000, HEAD = 0x2A000, FACE = 0x2C000, GEOMETRY = 0x2E000, PATH = 0x30000,
    LOADER_PATH = 0x31000, STALE_PATH = 0x32000, STREAM = 0x33000, ENTRY = 0x34000,
    TRI = 0x35000, NAME_REF = 0x36000, NAME = 0x37000, RECORD = 0x38000, STACK = 0x3A000,
    FS = 0x40000, TLS_ARRAY = 0x41000, TLS_DATA = 0x42000, TLS_INDEX = 0x1BBEB54,
    MODEL = 0x43000, MODEL_VTABLE = 0x44000, GET_PATH = 0x45000, GEOMETRY_SLOT = 0x46000,
    GEOMETRY_INFO = 0x47000, PARSE_STACK = STACK + 0x400, SELECTOR_STACK = STACK + 0x800,
    TRI_RECORDS = 0x48000, MORPH_ORDER = 0x49000, DECREMENT = 0x4A000,
    DESTROY_TRI = 0x4B000, TRI_TABLE = 0x4C000, NOSE_DOWN_REF = NAME_REF + 4, NOSE_DOWN = NAME + 16,
    TRI_RECORD_BYTES = 36, EXTENDED_RECORD_OFFSET = 12
};
enum {
    FRESH, CACHED, MALE, STALE_HEAD_PATH, STALE_PARSER_PATH, OTHER_ENTRY, OLD_GEOMETRY, UNVERIFIED_SOURCE,
    RETIRED, WRONG_RECORD, CHANGED_MENU, WRONG_CALLER, CASES
};

uint8_t *MEM;
static unsigned kind, begin_count, finish_count, reports, apply_calls;
static EceNoseWitnessReport result;
static CPU cpu;
static bool lookup_only, check_after_release;
static unsigned entry_releases, tri_releases, frees, selected;
static uint32_t up_ordinal, down_ordinal;
static uint16_t geometry_vertices;

void test_original_lookup(CPU *owner);
void test_original_lookup_only(CPU *owner);

static uint32_t record_address(uint32_t ordinal) {
    return ordinal < 0xB0 ? RECORD + ordinal * TRI_RECORD_BYTES : TRI_RECORDS;
}

void rt_call_ic(CPU *owner, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(owner == &cpu);
    const uint32_t sp = owner->r[4];
    if (target == DECREMENT) {
        const uint32_t object = M32(sp + 4);
        if (object == ENTRY) {
            assert(M32(sp) == 0x5A5CBC && ++entry_releases == 1 && M32(ENTRY) > 0);
        } else {
            assert(object == TRI + 4 && M32(sp) == 0x5A0678 && ++tri_releases == 1 && M32(TRI + 4) > 0);
        }
        owner->r[0] = --M32(object);
        owner->r[4] += 8;
        return;
    }
    if (target == DESTROY_TRI) {
        assert(M32(sp) == 0x5A0685 && owner->r[1] == TRI && tri_releases == 1);
        owner->r[4] += 4;
        return;
    }
    assert(target == GET_PATH && owner->r[1] == MODEL);
    owner->r[0] = kind == STALE_HEAD_PATH ? STALE_PATH : PATH;
    owner->r[4] += 4;
}

static bool read_word(void *context, uint32_t address, void *out, size_t count) {
    assert(context == MEM && address >= 0x10000 && address <= PLAYER + 4 - count);
    memcpy(out, MEM + address, count);
    return true;
}

static uint64_t begin_source(void *context, const CPU *owner, uint32_t path, uint32_t stream) {
    assert(context == MEM && owner == &cpu && path == LOADER_PATH && stream == STREAM);
    assert(++begin_count == 1);
    return 7;
}

static bool finish_source(void *context, const CPU *owner, uint64_t receipt) {
    assert(context == MEM && owner == &cpu && receipt == 7 && ++finish_count == 1);
    return kind != UNVERIFIED_SOURCE;
}

static void report(void *context, EceNoseWitnessReport observed) {
    assert(context == MEM && ++reports == 1);
    result = observed;
}

// Authored allocation and resource backing. The stock lifted wrapper runs
// unchanged except for its observation calls and uses this controlled input.
void f_59ED30(CPU *c) { c->r[4] += 8; }
void f_46C7C0(CPU *c) { c->r[4] += 12; }
void f_59EF90(CPU *c) {
    assert(M32(c->r[4] + 8) == STREAM);
    c->r[0] = 1;
    c->r[4] += 12;
}
void f_A48D60(CPU *c) {
    assert(M32(c->r[4] + 4) == 0x30);
    c->r[0] = ENTRY;
    c->r[4] += 16;
}
void f_5A0550(CPU *c) {
    assert(c->r[1] == ENTRY);
    M32(ENTRY + 0x2C) = TRI;
    c->r[0] = ENTRY;
    c->r[4] += 16;
}
void f_A487B0(CPU *c) {
    assert(lookup_only && M32(c->r[4]) == 0x5A5CD4 && M32(c->r[4] + 4) == ENTRY);
    assert(!M32(ENTRY) && !M32(ENTRY + 0x2C) && ++frees == 1);
    c->r[4] += 12;
}
void f_5A04B0(CPU *c) {
    assert(c->r[1] == ENTRY + 8 && !M32(ENTRY + 0x2C));
    c->r[4] += 4;
}
void f_A51C20(CPU *c) {
    assert(M32(c->r[4] + 4) == ENTRY + 4 && !M32(ENTRY + 4));
    c->r[4] += 4;
}
void f_59EAB0(CPU *c) { c->r[4] += 4; }
void f_59EC90(CPU *c) { c->r[4] += 4; }

// The original selector and both index helpers read an authored runtime
// record array. Only the separate ordinary morph observation invokes apply.
void f_A4BC20(CPU *c) { c->r[4] += 4; }
void f_A4C220(CPU *c) { c->r[4] += 4; }
void f_5A7120(CPU *c) { (void)c; abort(); }
void f_5A7170(CPU *c) {
    ++apply_calls;
    assert(!lookup_only && M32(c->r[4] + 4) == record_address(3) &&
           M32(c->r[4] + 8) == (kind == OLD_GEOMETRY ? GEOMETRY + 4 : GEOMETRY));
    c->r[4] += 4;
}

static void original_parse(void) {
    cpu = (CPU){0};
    cpu.r[4] = PARSE_STACK;
    cpu.fs_base = FS;
    M32(PARSE_STACK + 4) = STREAM;
    M32(PARSE_STACK + 8) = kind == STALE_PARSER_PATH ? STALE_PATH : LOADER_PATH;
    M32(PARSE_STACK + 12) = NAME_REF;
    M32(PARSE_STACK + 16) = 0;
    f_5A0710(&cpu);
    assert(cpu.r[4] == PARSE_STACK + 4 && cpu.r[0] == ENTRY && M32(TLS_DATA + 0x4AC) == 0);
}

void f_5A2B40(CPU *owner) {
    assert(owner == &cpu && M32(owner->r[4] + 4) == (kind == STALE_HEAD_PATH ? STALE_PATH : PATH));
    const CPU lookup_cpu = *owner;
    if (lookup_only) {
        const uint32_t output = M32(owner->r[4] + 8);
        const uint32_t options = M32(owner->r[4] + 12);
        assert(!M32(output) && M32(options) == geometry_vertices && !M8(options + 4));
    }
    if (kind != CACHED) original_parse();
    if (kind == RETIRED) ece_nose_retiring(ENTRY);
    *owner = lookup_cpu;
    M32(M32(owner->r[4] + 8)) = kind == OTHER_ENTRY ? ENTRY + 4 : ENTRY;
    owner->r[0] = 0;
    owner->r[4] += 4;
}

static void original_lookup(void) {
    cpu = (CPU){0};
    cpu.r[4] = STACK;
    M32(STACK) = 0x5A4133;
    M32(STACK + 4) = NAME_REF;
    M32(STACK + 8) = MODEL;
    M32(STACK + 12) = GEOMETRY_SLOT;
    M32(STACK + 16) = 0;
    M32(STACK + 20) = 1;
    if (lookup_only) {
        test_original_lookup_only(&cpu);
        assert(cpu.r[4] == STACK + 24);
    } else {
        test_original_lookup(&cpu);
        assert(cpu.r[4] == STACK - 0x24 && cpu.r[0] == 0);
    }
}

static void check_record(CPU *owner, uint32_t name, uint32_t ordinal) {
    CPU selector = *owner;
    selector.r[1] = TRI;
    selector.r[4] = SELECTOR_STACK;
    M32(SELECTOR_STACK + 4) = SELECTOR_STACK + 0x20;
    M32(SELECTOR_STACK + 8) = name;
    f_5A7570(&selector);
    assert(selector.r[0] == record_address(ordinal) && selector.r[4] == SELECTOR_STACK + 12);
    ece_nose_morph_record(true, M32(name), selector.r[0] != 0);
    ++selected;
}

void test_lookup_probe(CPU *owner, uint32_t entry) {
    assert(lookup_only && owner == &cpu && entry == ENTRY && M32(ENTRY + 0x2C) == TRI);
    assert(!entry_releases && !tri_releases && !frees);
    if (check_after_release) return;
    assert(ece_nose_morph(TRI, GEOMETRY));
    check_record(owner, NAME_REF, up_ordinal);
    check_record(owner, NOSE_DOWN_REF, down_ordinal);
}

static void original_morph(void) {
    cpu = (CPU){0};
    cpu.r[1] = TRI;
    cpu.r[4] = STACK;
    M32(STACK) = 0x5A5C79;
    M32(STACK + 4) = NAME_REF;
    M32(STACK + 8) = kind == OLD_GEOMETRY ? GEOMETRY + 4 : GEOMETRY;
    M32(STACK + 12) = 0;
    M32(STACK + 16) = 1;
    f_5A75F0(&cpu);
    assert(cpu.r[4] == STACK + 20);
}

static void test_case(void) {
    M8(BASE + 0x24) = kind != MALE;
    M8(MENU + 0x10) = 0x40;
    M32(record_address(3)) = kind == WRONG_RECORD ? NAME + 4 : NAME;
    begin_count = finish_count = reports = apply_calls = 0;
    assert(ece_nose_witness_arm(read_word, begin_source, finish_source, report, MEM));
    if (kind == WRONG_CALLER) {
        assert(!ece_nose_menu(0xA5CE2C, MENU));
        assert(!ece_nose_begin(MENU, BASE, RACE));
    }
    const bool menu = ece_nose_menu(0xA5CE2B, MENU);
    assert(menu);
    const bool nose = ece_nose_begin(MENU, BASE, RACE);
    assert(nose == (kind != MALE));
    ece_nose_face(0x885611, MENU, ACTOR, BASE, FACE);
    const bool head = ece_nose_head(0x5A48D2, FACE, HEAD, GEOMETRY);
    assert(head == nose);
    const bool lookup = nose && kind != STALE_HEAD_PATH;
    original_lookup();
    original_morph();
    assert(apply_calls == (kind != WRONG_RECORD));
    if (kind == CHANGED_MENU) M8(MENU + 0x10) = 0;
    ece_nose_head_end(head);
    ece_nose_end(nose);
    ece_nose_menu_end(menu);

    const bool expected_source = lookup && kind != CACHED && kind != STALE_PARSER_PATH;
    assert(begin_count == expected_source && finish_count == expected_source);
    const bool valid_menu = nose && kind != CHANGED_MENU;
    assert(reports == 1 && result.female_nord == valid_menu &&
           result.head == valid_menu && result.geometry == valid_menu);
    assert(result.selected_path == (valid_menu && lookup));
    assert(result.fresh_tri == (valid_menu && expected_source && kind != OTHER_ENTRY &&
                                kind != OLD_GEOMETRY && kind != UNVERIFIED_SOURCE && kind != RETIRED));
    assert(result.morph_record == (valid_menu && lookup && kind != OLD_GEOMETRY && kind != WRONG_RECORD));
}

static void test_both_records(bool after_release, bool cached_reference) {
    uint8_t geometry_snapshot[0xC0], info_snapshot[16];
    memcpy(geometry_snapshot, MEM + GEOMETRY, sizeof geometry_snapshot);
    memcpy(info_snapshot, MEM + GEOMETRY_INFO, sizeof info_snapshot);
    M8(MENU + 0x10) = 0x40;
    M8(BASE + 0x24) = 1;
    M32(0x106B154) = DECREMENT;
    M32(TRI) = 0x10BE9D4;
    M32(0x10BE9D4 + 4) = DESTROY_TRI;
    M32(TRI + 4) = 1;
    M32(ENTRY) = 1 + cached_reference;
    M32(TRI + 0x40) = 0;
    M32(TRI + 0x44) = MORPH_ORDER;
    M32(TRI + 0x48) = 2;
    M8(MORPH_ORDER) = (uint8_t)down_ordinal;
    M8(MORPH_ORDER + 1) = (uint8_t)up_ordinal;
    M32(record_address(up_ordinal)) = NAME;
    M32(record_address(down_ordinal)) = NOSE_DOWN;
    begin_count = finish_count = reports = apply_calls = 0;
    entry_releases = tri_releases = frees = selected = 0;
    kind = FRESH;
    check_after_release = after_release;
    lookup_only = true;

    assert(ece_nose_witness_arm(read_word, begin_source, finish_source, report, MEM));
    assert(ece_nose_menu(0xA5CE2B, MENU));
    assert(ece_nose_begin(MENU, BASE, RACE));
    ece_nose_face(0x885611, MENU, ACTOR, BASE, FACE);
    assert(ece_nose_head(0x5A48D2, FACE, HEAD, GEOMETRY));
    original_lookup();
    assert(!apply_calls && M32(ENTRY) == (unsigned)cached_reference && entry_releases == 1);
    assert(tri_releases == (unsigned)!cached_reference && frees == (unsigned)!cached_reference);
    assert(!memcmp(geometry_snapshot, MEM + GEOMETRY, sizeof geometry_snapshot));
    assert(!memcmp(info_snapshot, MEM + GEOMETRY_INFO, sizeof info_snapshot));
    if (after_release) {
        // Simulate storage reuse without dereferencing a stale object.
        M32(ENTRY + 0x2C) = TRI;
        assert(ece_nose_morph(TRI, GEOMETRY));
    }
    ece_nose_head_end(true);
    assert(!ece_nose_morph(TRI, GEOMETRY));
    ece_nose_end(true);
    ece_nose_menu_end(true);
    assert(begin_count == 1 && finish_count == 1 && reports == 1);
    assert(result.selected_path && result.fresh_tri == !after_release && result.morph_record == !after_release);
    assert(selected == (after_release ? 0u : 2u));
    lookup_only = false;
}

int main(int argc, char **argv) {
    MEM = calloc(1, TLS_INDEX + 4);
    assert(MEM);
    M32(FS + 0x2C) = TLS_ARRAY;
    M32(TLS_ARRAY) = TLS_DATA;
    M32(PLAYER) = ACTOR;
    M32(ACTOR) = 0x10D201C;
    M32(ACTOR + 0x24) = BASE;
    M32(ACTOR + 0x130) = RACE;
    M32(BASE + 0xC4) = RACE;
    M8(BASE + 0x12) = 0x2B;
    M32(RACE + 0x0C) = 0x13746;
    M8(RACE + 0x12) = 0x0E;
    M32(MENU) = 0x10E7404;
    M32(MENU + 0xF4) = 1;
    M32(MENU + 0xCC) = ROWS;
    M32(MENU + 0xD4) = 1;
    M32(ROWS) = RACE;
    M32(HEAD + 0x0C) = 0x51623;
    M8(HEAD + 0x12) = 0x0C;
    M32(HEAD + 0x3C) = 1;
    M32(TRI) = 0x10BE9D4;
    M32(TRI + 8) = TRI_TABLE;
    M32(TRI_TABLE) = RECORD;
    M32(TRI_TABLE + 8) = 0xB0;
    M32(TRI_TABLE + EXTENDED_RECORD_OFFSET) = TRI_RECORDS;
    M32(TRI_TABLE + 0x14) = 1;
    M32(TRI + 0x40) = 0x80000000;
    M8(TRI + 0x44) = 3;
    M32(TRI + 0x48) = 1;
    M32(NAME_REF) = NAME;
    M32(NOSE_DOWN_REF) = NOSE_DOWN;
    M32(MODEL) = MODEL_VTABLE;
    M32(MODEL_VTABLE + 0x10) = GET_PATH;
    M32(GEOMETRY_SLOT) = GEOMETRY;
    M32(GEOMETRY + 0xB0) = GEOMETRY_INFO;
    strcpy((char *)MEM + NAME, "NoseUp");
    strcpy((char *)MEM + NOSE_DOWN, "NoseDown");
    strcpy((char *)MEM + PATH, "Actors\\Character\\Character Assets\\FemaleHeadCharGen.tri");
    strcpy((char *)MEM + LOADER_PATH, "meshes/actors/character/character assets/femaleheadchargen.tri");
    strcpy((char *)MEM + STALE_PATH, "Actors\\Character\\Character Assets\\FemaleHead.tri");
    assert(argc == 1 || argc == 3);
    geometry_vertices = argc == 1 ? 9 : 996;
    M16(GEOMETRY_INFO + 8) = geometry_vertices;
    if (argc == 1) {
        for (kind = 0; kind < CASES; ++kind) test_case();
        up_ordinal = 3;
        down_ordinal = 0xB0;
    } else {
        up_ordinal = (uint32_t)atoi(argv[1]);
        down_ordinal = (uint32_t)atoi(argv[2]);
        assert(up_ordinal == 13 && down_ordinal == 12);
    }
    test_both_records(false, false);
    test_both_records(true, false);
    test_both_records(false, true);
    free(MEM);
    return 0;
}
