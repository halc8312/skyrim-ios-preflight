/* SPDX-License-Identifier: GPL-3.0-only */
#include "skyui_density.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const char keys[] = "_global.skyui.util.ConfigManager.setExternalOverrideKeys";
static const char values[] = "_global.skyui.util.ConfigManager.setExternalOverrideValues";

typedef struct { int unused; } CPU;
typedef struct { const char *menu, *target; unsigned operation, count; } UiCommand;
enum { S_UI_ASTRING = 17 };

static _Alignas(16) unsigned char memory[0x80000];
#define GPTR(a) (memory + (a))
#define M8(a) (memory[(a)])
#define M32(a) (*(uint32_t *)(void *)GPTR(a))
#define MF64(a) (*(double *)(void *)GPTR(a))

static uint32_t next = 0x10000;
static unsigned allocated, retain_count, argv_count, reads, invokes, view_id;
static unsigned phase = 1;
static int fail_index = -1;
static const char *array_keys[SKYUI_DENSITY_MAX_EXTRAS];
static unsigned array_count;
static char sent_first_extra[96];

static uint32_t gmalloc(size_t bytes) {
    const uint32_t address = next;
    next += (uint32_t)((bytes + 15) & ~(size_t)15);
    assert(next < sizeof memory);
    ++allocated;
    return address;
}
static void gfree(uint32_t address) {
    assert(address >= 0x10000 && address < next && allocated);
    --allocated;
}
static uint32_t gstrdup(const char *text) {
    const uint32_t address = gmalloc(strlen(text) + 1);
    strcpy((char *)GPTR(address), text);
    return address;
}

static void skse_gfx_read(CPU *cpu, uint32_t view, const char *path, uint32_t out) {
    (void)cpu;
    ++reads;
    assert(view == view_id);
    if (strstr(path, "._loadPhase")) {
        M32(out + 4) = 3;
        MF64(out + 8) = phase;
    } else {
        assert(strstr(path, ".in_overrideKeys"));
        M32(out) = 0x11000;
        M32(out + 4) = 0x47;
        M32(out + 8) = 0x12000;
        ++retain_count;
    }
}

static void skse_gfx_clean(CPU *cpu, uint32_t value) {
    (void)cpu;
    if (M32(value + 4) == 0x47) {
        assert(retain_count);
        --retain_count;
    }
    memset(GPTR(value), 0, 16);
}

static uint32_t skse_member(CPU *cpu, uint32_t address, uint32_t iface, unsigned argc, const uint32_t *args) {
    (void)cpu;
    assert(iface == 0x11000 && args[0] == 0x12000);
    if (address == 0x920E80) {
        assert(argc == 1);
        return array_count;
    }
    assert(address == 0x923AD0 && argc == 3 && args[1] >= SKYUI_OVERRIDE_COUNT && args[1] < array_count);
    unsigned extra = args[1] - SKYUI_OVERRIDE_COUNT;
    const uint32_t scratch = args[2], string = 0x13000;
    M32(scratch + 4) = 4;
    M32(scratch + 8) = string;
    strcpy((char *)GPTR(string), array_keys[extra]);
    return extra == (unsigned)fail_index ? 0xffffff00u : 0xffffff01u;
}

static void skse_virtual(CPU *cpu, uint32_t view, unsigned slot, unsigned count, const uint32_t *args) {
    (void)cpu;
    assert(view == view_id && slot == 23 && count == 4 && args[1] == 0);
    assert(M32(args[2] + 4) == 3 && MF64(args[2] + 8) == 12.0); /* original GFXValue copied verbatim */
    argv_count = args[3];
    assert(argv_count > SKYUI_OVERRIDE_COUNT);
    uint32_t appended = args[2] + 16 * SKYUI_OVERRIDE_COUNT;
    assert(M32(appended + 4) == 4);
    const char *text = (const char *)GPTR(M32(appended + 8));
    assert(strlen(text) < sizeof sent_first_extra);
    strcpy(sent_first_extra, text);
    ++invokes;
}

static uint32_t skse_menu_view(const char *menu) {
    assert(!strcmp(menu, "MagicMenu"));
    return view_id;
}

#include "classic_skyui_density.c.in"

static const char *find(const SkyrimSkyUIOverride *entries, size_t count, const char *key) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(entries[i].key, key)) return entries[i].value;
    return NULL;
}

int main(void) {
    SkyrimSkyUIOverride entries[SKYUI_DENSITY_MAX_EXTRAS];
    assert(skyrim_skyui_density_message(keys, 128) == SKYUI_OVERRIDE_KEYS);
    assert(skyrim_skyui_density_message(values, 128) == SKYUI_OVERRIDE_VALUES);
    assert(skyrim_skyui_density_message(values, 127) == SKYUI_IGNORE);
    assert(skyrim_skyui_density_message(".setExternalOverrideValues", 128) == SKYUI_IGNORE);
    const size_t inventory = skyrim_skyui_density_plan(SKYUI_LARGE_TEXT, "InventoryMenu", entries);
    assert(inventory == 16);
    assert(!strcmp(find(entries, inventory, "ListLayout$defaults$entry$textFormat$size"), "18"));
    assert(!strcmp(find(entries, inventory, "ListLayout$columns$weightColumn$hidden"), "false"));
    assert(!strcmp(find(entries, inventory, "ListLayout$views$armorView$columns"),
                   "<equipColumn,iconColumn,itemNameColumn,weightColumn,valueColumn>"));
    assert(!find(entries, inventory, "ListLayout$views$allMagicView$columns"));

    const size_t magic = skyrim_skyui_density_plan(SKYUI_COMPACT, "MagicMenu", entries);
    assert(magic == 13);
    assert(!strcmp(find(entries, magic, "ListLayout$defaults$entry$textFormat$size"), "13"));
    assert(!strcmp(find(entries, magic, "ListLayout$views$allMagicView$columns"),
                   "<equipColumn,iconColumn,magicNameColumn,spellCostColumn,schoolColumn>"));
    assert(!find(entries, magic, "ListLayout$views$shoutsView$columns"));
    assert(!find(entries, magic, "ListLayout$views$activeEffectView$columns"));
    assert(!find(entries, magic, "ListLayout$columns$weightColumn$width"));

    SkyrimSkyUIOverride before[SKYUI_DENSITY_MAX_EXTRAS];
    memcpy(before, entries, sizeof before);
    assert(!skyrim_skyui_density_plan(SKYUI_STANDARD, "MagicMenu", entries));
    assert(!skyrim_skyui_density_plan(SKYUI_COMPACT, "BarterMenu", entries));
    assert(!memcmp(before, entries, sizeof before));

    uint32_t original = gmalloc(16 * SKYUI_OVERRIDE_COUNT);
    M32(original + 4) = 3;
    MF64(original + 8) = 12.0;
    CPU cpu = {0};
    UiCommand command = {"MagicMenu", keys, S_UI_ASTRING, 128};
    view_id = 13;
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    assert(!reads && !invokes);
    skyrim_skyui_density_configure(SKYUI_COMPACT);
    assert(density_invoke(&cpu, &command, view_id, 0, original));
    assert(argv_count == SKYUI_OVERRIDE_COUNT + magic && !strcmp(sent_first_extra, entries[0].key));
    assert(!reads && invokes == 1 && allocated == 1);

    command.target = values;
    array_count = SKYUI_OVERRIDE_COUNT;
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    array_count += (unsigned)magic;
    for (unsigned i = 0; i < magic; ++i) array_keys[i] = entries[i].key;
    assert(density_invoke(&cpu, &command, view_id, 0, original));
    assert(argv_count == SKYUI_OVERRIDE_COUNT + magic && !strcmp(sent_first_extra, entries[0].value));
    assert(invokes == 2 && !retain_count && allocated == 1);

    phase = 0;
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    phase = 2;
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    phase = 1;
    fail_index = 1;
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    fail_index = -1;
    array_keys[2] = "unexpected";
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    array_keys[2] = entries[2].key;
    ++view_id; /* a replacement movie has its own, empty array */
    array_count = 0;
    assert(!density_invoke(&cpu, &command, view_id, 0, original));
    assert(invokes == 2 && !retain_count && allocated == 1);
    gfree(original);
    return 0;
}
