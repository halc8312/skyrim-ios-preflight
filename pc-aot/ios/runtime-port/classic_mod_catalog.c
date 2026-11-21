/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_mod_catalog.h"
#include "classic_mod_layout.h"

#include <stdlib.h>
#include <string.h>

struct ClassicModCatalog {
    size_t count;
    SkseLoadedMod entries[];
};

enum { GUEST_MIN_ADDRESS = 0x10000, GUEST_POINTER_BYTES = 4 };

_Static_assert(CLASSIC_FILE_NAME_BYTES == SKSE_CLASSIC_MOD_NAME_LIMIT + 1, "Classic filename width");
_Static_assert(CLASSIC_LOADED_MODS == CLASSIC_LOADED_COUNT + GUEST_POINTER_BYTES, "Classic active array header");

static bool read_at(ClassicModCatalogRead read, void *context, uint32_t base, unsigned offset,
                    void *bytes, size_t length) {
    uint64_t address = (uint64_t)base + offset;
    return base >= GUEST_MIN_ADDRESS && address + length <= UINT64_C(0x100000000) &&
           read(context, (uint32_t)address, bytes, length);
}

static uint32_t read_le32(const uint8_t bytes[GUEST_POINTER_BYTES]) {
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static size_t plugin_name_length(const uint8_t name[CLASSIC_FILE_NAME_BYTES]) {
    const uint8_t *end = memchr(name, 0, CLASSIC_FILE_NAME_BYTES);
    if (!end || end - name <= 4) return 0;
    size_t length = (size_t)(end - name);
    static const uint8_t master[] = ".esm", plugin[] = ".esp";
    if (skse_classic_mod_name_compare(end - 4, 4, master, 4) &&
        skse_classic_mod_name_compare(end - 4, 4, plugin, 4)) return 0;
    /* The bytes are copied for comparison, never used as paths. In the
     * game's code page, 0x5c can be part of a multibyte character. */
    for (size_t i = 0; i < length; ++i)
        if (name[i] < 0x20 || name[i] == '/' || name[i] == ':') return 0;
    return length;
}

static int compare_entry_names(const void *a, const void *b) {
    const SkseLoadedMod *x = *(const SkseLoadedMod *const *)a;
    const SkseLoadedMod *y = *(const SkseLoadedMod *const *)b;
    return skse_classic_mod_name_compare(x->name, x->length, y->name, y->length);
}

static ClassicModCatalogResult copy_rows(ClassicModCatalogRead read, void *context,
                                        const uint8_t *pointers, ClassicModCatalog *catalog) {
    const SkseLoadedMod *names[SKSE_CLASSIC_MOD_LIMIT];
    uint8_t *name = (uint8_t *)(catalog->entries + catalog->count);
    for (size_t i = 0; i < catalog->count; ++i, name += CLASSIC_FILE_NAME_BYTES) {
        uint32_t file = read_le32(pointers + i * GUEST_POINTER_BYTES);
        uint8_t index;
        if (!read_at(read, context, file, CLASSIC_FILE_INDEX, &index, sizeof index) ||
            !read_at(read, context, file, CLASSIC_FILE_NAME, name, CLASSIC_FILE_NAME_BYTES))
            return CLASSIC_MOD_CATALOG_UNAVAILABLE;
        size_t length = plugin_name_length(name);
        if (index != i || !length) return CLASSIC_MOD_CATALOG_INVALID;
        catalog->entries[i] = (SkseLoadedMod){name, length, index, SKSE_LOADED_CLASSIC};
        names[i] = &catalog->entries[i];
    }
    qsort(names, catalog->count, sizeof names[0], compare_entry_names);
    for (size_t i = 1; i < catalog->count; ++i)
        if (!compare_entry_names(&names[i - 1], &names[i])) return CLASSIC_MOD_CATALOG_INVALID;
    return CLASSIC_MOD_CATALOG_OK;
}

static ClassicModCatalogResult verify_copy(ClassicModCatalogRead read, void *context, uint32_t handler,
                                            const uint8_t *pointers, const ClassicModCatalog *catalog) {
    for (size_t i = 0; i < catalog->count; ++i) {
        const SkseLoadedMod *entry = &catalog->entries[i];
        uint32_t file = read_le32(pointers + i * GUEST_POINTER_BYTES);
        uint8_t name[CLASSIC_FILE_NAME_BYTES], index;
        if (!read_at(read, context, file, CLASSIC_FILE_INDEX, &index, sizeof index) ||
            !read_at(read, context, file, CLASSIC_FILE_NAME, name, entry->length + 1))
            return CLASSIC_MOD_CATALOG_UNAVAILABLE;
        if (index != entry->index || memcmp(name, entry->name, entry->length + 1))
            return CLASSIC_MOD_CATALOG_CHANGED;
    }
    uint8_t singleton[GUEST_POINTER_BYTES], active[GUEST_POINTER_BYTES + SKSE_CLASSIC_MOD_LIMIT * GUEST_POINTER_BYTES];
    const size_t width = catalog->count * GUEST_POINTER_BYTES;
    if (!read_at(read, context, handler, CLASSIC_LOADED_COUNT, active, GUEST_POINTER_BYTES + width) ||
        !read_at(read, context, CLASSIC_DATA_HANDLER, 0, singleton, sizeof singleton))
        return CLASSIC_MOD_CATALOG_UNAVAILABLE;
    if (read_le32(singleton) != handler || read_le32(active) != catalog->count ||
        memcmp(active + GUEST_POINTER_BYTES, pointers, width)) return CLASSIC_MOD_CATALOG_CHANGED;
    return CLASSIC_MOD_CATALOG_OK;
}

ClassicModCatalogResult classic_mod_catalog_capture(ClassicModCatalogRead read, void *context, ClassicModCatalog **out) {
    if (!read || !out || *out) return CLASSIC_MOD_CATALOG_ARGUMENT;
    uint8_t singleton[GUEST_POINTER_BYTES], count_bytes[GUEST_POINTER_BYTES];
    uint8_t pointers[SKSE_CLASSIC_MOD_LIMIT * GUEST_POINTER_BYTES];
    if (!read_at(read, context, CLASSIC_DATA_HANDLER, 0, singleton, sizeof singleton))
        return CLASSIC_MOD_CATALOG_UNAVAILABLE;
    uint32_t handler = read_le32(singleton);
    if (!read_at(read, context, handler, CLASSIC_LOADED_COUNT, count_bytes, sizeof count_bytes))
        return CLASSIC_MOD_CATALOG_UNAVAILABLE;
    uint32_t count = read_le32(count_bytes);
    if (!count) return CLASSIC_MOD_CATALOG_UNAVAILABLE;
    if (count > SKSE_CLASSIC_MOD_LIMIT) return CLASSIC_MOD_CATALOG_INVALID;
    if (!read_at(read, context, handler, CLASSIC_LOADED_MODS, pointers, count * GUEST_POINTER_BYTES))
        return CLASSIC_MOD_CATALOG_UNAVAILABLE;

    ClassicModCatalog *catalog = malloc(sizeof *catalog + count * (sizeof catalog->entries[0] + CLASSIC_FILE_NAME_BYTES));
    if (!catalog) return CLASSIC_MOD_CATALOG_NO_MEMORY;
    catalog->count = count;
    ClassicModCatalogResult result = copy_rows(read, context, pointers, catalog);
    if (result == CLASSIC_MOD_CATALOG_OK) result = verify_copy(read, context, handler, pointers, catalog);
    if (result == CLASSIC_MOD_CATALOG_OK) *out = catalog;
    else free(catalog);
    return result;
}

const SkseLoadedMod *classic_mod_catalog_entries(const ClassicModCatalog *catalog, size_t *count) {
    if (count) *count = catalog ? catalog->count : 0;
    return catalog ? catalog->entries : NULL;
}

void classic_mod_catalog_free(ClassicModCatalog *catalog) { free(catalog); }
