/* SPDX-License-Identifier: GPL-3.0-only */
#include "skyui_density.h"
#include <stdatomic.h>
#include <string.h>

static atomic_uint selected_mode;

void skyrim_skyui_density_configure(enum SkyrimSkyUIDensity mode) {
    atomic_store_explicit(&selected_mode, mode == SKYUI_COMPACT || mode == SKYUI_LARGE_TEXT ? mode : SKYUI_STANDARD,
                          memory_order_relaxed);
}

enum SkyrimSkyUIDensity skyrim_skyui_density_mode(void) {
    return (enum SkyrimSkyUIDensity)atomic_load_explicit(&selected_mode, memory_order_relaxed);
}

enum SkyrimSkyUIMessage skyrim_skyui_density_message(const char *target, size_t count) {
    if (!target || count != SKYUI_OVERRIDE_COUNT) return SKYUI_IGNORE;
    if (!strcmp(target, "_global.skyui.util.ConfigManager.setExternalOverrideKeys")) return SKYUI_OVERRIDE_KEYS;
    if (!strcmp(target, "_global.skyui.util.ConfigManager.setExternalOverrideValues")) return SKYUI_OVERRIDE_VALUES;
    return SKYUI_IGNORE;
}

#define LIST "ListLayout$"
static const struct { const char *key, *compact, *large; } sizes[] = {
    {LIST "defaults$label$textFormat$size", "12", "14"},
    {LIST "defaults$entry$textFormat$size", "13", "18"},
    {LIST "vars$n_iconSize$value", "16", "20"},
    {LIST "vars$a_textBorder$value", "<0,0,0.3,0>", "<0,0,0.4,0>"},
    {LIST "columns$equipColumn$indent", "-25", "-30"},
    {LIST "columns$equipColumn$border", "<0,10,2,2>", "<0,10,3.2,3.2>"},
    {LIST "columns$iconColumn$border", "<0,3,2,2>", "<0,4,3.2,3.2>"},
};

#define ITEM_COLUMNS "<equipColumn,iconColumn,itemNameColumn,weightColumn,valueColumn>"
static const SkyrimSkyUIOverride inventory[] = {
    {LIST "views$defaultItemView$columns", ITEM_COLUMNS},
    {LIST "views$weaponView$columns", ITEM_COLUMNS},
    {LIST "views$armorView$columns", ITEM_COLUMNS},
    {LIST "views$magicItemView$columns", ITEM_COLUMNS},
    {LIST "views$keysView$columns", ITEM_COLUMNS},
    {LIST "columns$weightColumn$hidden", "false"},
    {LIST "columns$valueColumn$hidden", "false"},
    {LIST "columns$weightColumn$width", "0.13"},
    {LIST "columns$valueColumn$width", "0.14"},
};
#undef ITEM_COLUMNS

#define SPELL_COLUMNS "<equipColumn,iconColumn,magicNameColumn,spellCostColumn,schoolColumn>"
static const SkyrimSkyUIOverride magic[] = {
    {LIST "views$allMagicView$columns", SPELL_COLUMNS},
    {LIST "views$defaultMagicView$columns", SPELL_COLUMNS},
    {LIST "columns$spellCostColumn$hidden", "false"},
    {LIST "columns$schoolColumn$hidden", "false"},
    {LIST "columns$spellCostColumn$width", "0.17"},
    {LIST "columns$schoolColumn$width", "0.20"},
};
#undef SPELL_COLUMNS
#undef LIST

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
_Static_assert(ARRAY_COUNT(sizes) + ARRAY_COUNT(inventory) <= SKYUI_DENSITY_MAX_EXTRAS, "Inventory extras fit");
_Static_assert(ARRAY_COUNT(sizes) + ARRAY_COUNT(magic) <= SKYUI_DENSITY_MAX_EXTRAS, "Magic extras fit");

size_t skyrim_skyui_density_plan(enum SkyrimSkyUIDensity mode, const char *menu,
    SkyrimSkyUIOverride out[SKYUI_DENSITY_MAX_EXTRAS]) {
    if ((mode != SKYUI_COMPACT && mode != SKYUI_LARGE_TEXT) || !menu || !out) return 0;

    const SkyrimSkyUIOverride *view_overrides;
    size_t view_count;
    if (!strcmp(menu, "InventoryMenu")) {
        view_overrides = inventory;
        view_count = ARRAY_COUNT(inventory);
    } else if (!strcmp(menu, "MagicMenu")) {
        view_overrides = magic;
        view_count = ARRAY_COUNT(magic);
    } else return 0;

    for (size_t i = 0; i < ARRAY_COUNT(sizes); ++i) {
        out[i].key = sizes[i].key;
        out[i].value = mode == SKYUI_COMPACT ? sizes[i].compact : sizes[i].large;
    }
    memcpy(out + ARRAY_COUNT(sizes), view_overrides, view_count * sizeof(*view_overrides));
    return ARRAY_COUNT(sizes) + view_count;
}
