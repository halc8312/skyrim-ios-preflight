/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_mod_catalog.h"
#include "classic_mod_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { HANDLER = 0x10000, STACK = 0x17000, FILE_BASE = 0x20000, FILE_STRIDE = 0x500 };
static uint8_t memory[CLASSIC_DATA_HANDLER + sizeof(uint32_t)];
static uint32_t refused, change_address, change_at;
static unsigned change_read, hits, attempts;

#ifdef CLASSIC_TEST_PINNED
#include "x86.h"
uint8_t *MEM = memory;
void f_43B650(CPU *);
void f_43B660(CPU *);
void f_444830(CPU *);
#endif

static void put_word(uint32_t address, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) memory[address + i] = (uint8_t)(value >> (i * 8));
}

static uint32_t file_at(unsigned index) { return FILE_BASE + index * FILE_STRIDE; }

static void set_name(unsigned index, const char *name) {
    strcpy((char *)memory + file_at(index) + CLASSIC_FILE_NAME, name);
}

static void set_count(unsigned count) { put_word(HANDLER + CLASSIC_LOADED_COUNT, count); }

static void reset_fixture(unsigned count) {
    refused = change_address = change_at = 0;
    attempts = change_read = hits = 0;
    put_word(CLASSIC_DATA_HANDLER, HANDLER);
    set_count(count);
    /* The linked-list order and contents differ from the active array. */
    put_word(HANDLER + CLASSIC_MOD_LIST, file_at(count));
    memset(memory + FILE_BASE, 0, (count + 1) * FILE_STRIDE);
    set_name(count, "Inactive.esp");
    for (unsigned i = 0; i < count; ++i) {
        char name[30];
        assert(snprintf(name, sizeof name, "Fixture%03u.esp", i) > 0);
        set_name(i, name);
        put_word(HANDLER + CLASSIC_LOADED_MODS + i * 4, file_at(i));
        memory[file_at(i) + CLASSIC_FILE_INDEX] = (uint8_t)i;
    }
}

static bool read_fixture(void *context, uint32_t address, void *out, size_t size) {
    assert(context == memory && address >= HANDLER && size && (uint64_t)address + size <= UINT64_C(0x100000000));
    ++attempts;
    if (address == change_address && ++hits == change_read) memory[change_at] ^= 1;
    if ((uint64_t)address + size > sizeof memory || address == refused) return false;
    memcpy(out, memory + address, size);
    return true;
}

static void expect_failure(ClassicModCatalogResult expected) {
    ClassicModCatalog *catalog = NULL;
    assert(classic_mod_catalog_capture(read_fixture, memory, &catalog) == expected && !catalog);
}

static void valid_catalog(void) {
    reset_fixture(2);
    set_name(0, "Skyrim.esm");
    static const char western_or_japanese[] = "\x95\\" "Shape.EsP";
    set_name(1, western_or_japanese);
    ClassicModCatalog *catalog = NULL;
    assert(classic_mod_catalog_capture(read_fixture, memory, &catalog) == CLASSIC_MOD_CATALOG_OK);
    assert(catalog && attempts == 13);
    size_t count;
    const SkseLoadedMod *entries = classic_mod_catalog_entries(catalog, &count);
    assert(count == 2 && entries[0].kind == SKSE_LOADED_CLASSIC && entries[1].kind == SKSE_LOADED_CLASSIC);
    assert(entries[0].index == 0 && entries[1].index == 1);
    assert(entries[0].length == sizeof "Skyrim.esm" - 1);
    assert(!memcmp(entries[0].name, "Skyrim.esm", sizeof "Skyrim.esm"));
    assert(entries[1].length == sizeof western_or_japanese - 1);
    assert(!memcmp(entries[1].name, western_or_japanese, sizeof western_or_japanese));
    set_name(0, "Overwritten.esm");
    set_name(1, "Overwritten.esp");
    assert(!memcmp(entries[0].name, "Skyrim.esm", sizeof "Skyrim.esm"));
    assert(!memcmp(entries[1].name, western_or_japanese, sizeof western_or_japanese));
    ClassicModCatalog *previous = catalog;
    assert(classic_mod_catalog_capture(read_fixture, memory, &catalog) == CLASSIC_MOD_CATALOG_ARGUMENT);
    assert(catalog == previous);
    classic_mod_catalog_free(catalog);
}

static void boundaries(void) {
    reset_fixture(SKSE_CLASSIC_MOD_LIMIT);
    char maximum[CLASSIC_FILE_NAME_BYTES];
    memset(maximum, 'a', sizeof maximum - 5);
    memcpy(maximum + sizeof maximum - 5, ".esp", 5);
    set_name(254, maximum);
    ClassicModCatalog *catalog = NULL;
    assert(classic_mod_catalog_capture(read_fixture, memory, &catalog) == CLASSIC_MOD_CATALOG_OK);
    size_t count;
    const SkseLoadedMod *entries = classic_mod_catalog_entries(catalog, &count);
    assert(count == SKSE_CLASSIC_MOD_LIMIT);
    assert(entries[254].index == 254 && entries[254].length == SKSE_CLASSIC_MOD_NAME_LIMIT);
    assert(!memcmp(entries[254].name, maximum, sizeof maximum));
    classic_mod_catalog_free(catalog);

    reset_fixture(0);
    expect_failure(CLASSIC_MOD_CATALOG_UNAVAILABLE);
    set_count(SKSE_CLASSIC_MOD_LIMIT + 1);
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    put_word(CLASSIC_DATA_HANDLER, UINT32_MAX - CLASSIC_LOADED_MODS + 1u);
    expect_failure(CLASSIC_MOD_CATALOG_UNAVAILABLE);
    put_word(CLASSIC_DATA_HANDLER, 0);
    expect_failure(CLASSIC_MOD_CATALOG_UNAVAILABLE);
    assert(!classic_mod_catalog_entries(NULL, &count) && !count);
}

static void broken_rows(void) {
    reset_fixture(2);
    memory[file_at(1) + CLASSIC_FILE_INDEX] = 0;
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    memory[file_at(1) + CLASSIC_FILE_INDEX] = 255;
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    memory[file_at(1) + CLASSIC_FILE_INDEX] = 1;
    set_name(1, "fixture000.EsP");
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    set_name(1, ".esp");
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    set_name(1, "Fixture.esl");
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    set_name(1, "Folder/Fixture.esp");
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    memset(memory + file_at(1) + CLASSIC_FILE_NAME, 'a', CLASSIC_FILE_NAME_BYTES);
    expect_failure(CLASSIC_MOD_CATALOG_INVALID);
    put_word(HANDLER + CLASSIC_LOADED_MODS + 4, UINT32_MAX);
    expect_failure(CLASSIC_MOD_CATALOG_UNAVAILABLE);
    put_word(HANDLER + CLASSIC_LOADED_MODS + 4, 0);
    expect_failure(CLASSIC_MOD_CATALOG_UNAVAILABLE);
    put_word(HANDLER + CLASSIC_LOADED_MODS + 4, file_at(1));
    refused = file_at(1) + CLASSIC_FILE_NAME;
    expect_failure(CLASSIC_MOD_CATALOG_UNAVAILABLE);
}

static void changing_rows(void) {
    const uint32_t changing[] = {
        file_at(0) + CLASSIC_FILE_NAME, file_at(1) + CLASSIC_FILE_INDEX, HANDLER + CLASSIC_LOADED_COUNT,
        HANDLER + CLASSIC_LOADED_MODS + 4, CLASSIC_DATA_HANDLER
    };
    for (size_t i = 0; i < sizeof changing / sizeof changing[0]; ++i) {
        reset_fixture(2);
        change_address = i == 3 ? HANDLER + CLASSIC_LOADED_COUNT : changing[i];
        change_at = changing[i];
        change_read = 2;
        expect_failure(CLASSIC_MOD_CATALOG_CHANGED);
    }
}

#ifdef CLASSIC_TEST_PINNED
static void pinned_accessors(void) {
    reset_fixture(3);
    CPU cpu = {0};
    cpu.r[1] = HANDLER;
    cpu.r[4] = STACK;
    f_43B650(&cpu);
    assert(cpu.r[0] == 3 && cpu.r[4] == STACK + 4);
    for (unsigned i = 0; i < 3; ++i) {
        memory[file_at(i) + CLASSIC_FILE_INDEX] = 255;
        cpu.r[1] = HANDLER;
        cpu.r[4] = STACK;
        put_word(STACK + 4, i);
        f_43B660(&cpu);
        assert(cpu.r[0] == file_at(i) && cpu.r[4] == STACK + 8);
        cpu.r[1] = cpu.r[0];
        cpu.r[4] = STACK;
        f_444830(&cpu);
        assert(memory[file_at(i) + CLASSIC_FILE_INDEX] == i && cpu.r[4] == STACK + 8);
    }
    CPU before;
    memcpy(&before, &cpu, sizeof before);
    ClassicModCatalog *catalog = NULL;
    assert(classic_mod_catalog_capture(read_fixture, memory, &catalog) == CLASSIC_MOD_CATALOG_OK);
    assert(!memcmp(&before, &cpu, sizeof before));
    size_t count;
    const SkseLoadedMod *entries = classic_mod_catalog_entries(catalog, &count);
    assert(count == 3);
    for (size_t i = 0; i < count; ++i) {
        assert(entries[i].index == i);
        assert(!memcmp(entries[i].name, memory + file_at((unsigned)i) + CLASSIC_FILE_NAME, entries[i].length + 1));
    }
    classic_mod_catalog_free(catalog);
}
#endif

int main(void) {
    valid_catalog();
    boundaries();
    broken_rows();
    changing_rows();
#ifdef CLASSIC_TEST_PINNED
    pinned_accessors();
#endif
    return 0;
}
