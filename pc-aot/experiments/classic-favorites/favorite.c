/* SPDX-License-Identifier: GPL-3.0-only */
#include "favorite.h"

enum {
    FORM_TYPE = 0x12,
    FORM_SPELL = 22,
    FORM_SHOUT = 119,
    PLAYER_EXTRA_LIST = 0x48,
    MAGIC_FAVORITE_ARRAY = 0x08,
    CONTAINER_CHANGES = 0x15,
    EXTRA_HOTKEY = 0x49,
};

static bool read_field(const FavoriteMemory *memory, uint32_t base, uint64_t offset,
                       void *dest, size_t size) {
    if (!base || offset + size > (uint64_t)UINT32_MAX + 1 - base) return false;
    return memory->read(memory->context, base + (uint32_t)offset, dest, size);
}

static bool read_word(const FavoriteMemory *memory, uint32_t base, uint64_t offset,
                      uint32_t *value) {
    uint8_t bytes[4];
    if (!read_field(memory, base, offset, bytes, sizeof(bytes))) return false;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
             (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

static enum FavoriteResult magic_contains(const FavoriteMemory *memory, uint32_t manager,
                                          uint32_t form) {
    if (!manager) return FAVORITE_NO;
    uint32_t count, capacity, array;
    if (!read_word(memory, manager, MAGIC_FAVORITE_ARRAY + 8, &count)) return FAVORITE_INVALID;
    if (!count) return FAVORITE_NO;
    if (!read_word(memory, manager, MAGIC_FAVORITE_ARRAY + 4, &capacity) || count > capacity ||
        !read_word(memory, manager, MAGIC_FAVORITE_ARRAY, &array) ||
        !array || (uint64_t)count * 4 > (uint64_t)UINT32_MAX + 1 - array) {
        return FAVORITE_INVALID;
    }
    uint32_t first = 0, limit = count;
    while (first < limit) {
        uint32_t middle = first + (limit - first - 1) / 2;
        uint32_t value;
        if (!read_word(memory, array, (uint64_t)middle * 4, &value)) return FAVORITE_INVALID;
        if (value == form) return FAVORITE_YES;
        if (value < form) first = middle + 1;
        else limit = middle;
    }
    return FAVORITE_NO;
}

/* A linear walk with constant space. The checkpoint detects corrupt cycles
 * without an extra read or a fixed cap on legitimate inventory sizes. */
typedef struct {
    uint32_t node;
    uint32_t checkpoint;
    uint64_t interval;
    uint64_t steps;
} ListCursor;

static bool advance(const FavoriteMemory *memory, ListCursor *cursor) {
    uint32_t next;
    if (!read_word(memory, cursor->node, 4, &next) || next == cursor->checkpoint) return false;
    cursor->node = next;
    if (++cursor->steps == cursor->interval) {
        cursor->checkpoint = next;
        cursor->interval *= 2;
        cursor->steps = 0;
    }
    return true;
}

static enum FavoriteResult instance_has_hotkey(const FavoriteMemory *memory, uint32_t list) {
    ListCursor cursor = {list, list, 1, 0};
    while (cursor.node) {
        uint32_t instance, hotkey;
        if (!read_word(memory, cursor.node, 0, &instance)) return FAVORITE_INVALID;
        /* An empty element ends the Classic extend-data list's GetNthItem walk. */
        if (!instance) break;
        if (!memory->extra(memory->context, instance, EXTRA_HOTKEY, &hotkey)) {
            return FAVORITE_INVALID;
        }
        if (hotkey) return FAVORITE_YES;
        if (!advance(memory, &cursor)) return FAVORITE_INVALID;
    }
    return FAVORITE_NO;
}

static enum FavoriteResult inventory_contains(const FavoriteMemory *memory, uint32_t player,
                                              uint32_t form) {
    uint32_t changes, data, entries;
    if (player > UINT32_MAX - PLAYER_EXTRA_LIST ||
        !memory->extra(memory->context, player + PLAYER_EXTRA_LIST, CONTAINER_CHANGES, &changes)) {
        return FAVORITE_INVALID;
    }
    if (!changes) return FAVORITE_NO;
    if (!read_word(memory, changes, 8, &data)) return FAVORITE_INVALID;
    if (!data) return FAVORITE_NO;
    if (!read_word(memory, data, 0, &entries)) return FAVORITE_INVALID;
    ListCursor cursor = {entries, entries, 1, 0};
    while (cursor.node) {
        uint32_t entry, item;
        if (!read_word(memory, cursor.node, 0, &entry)) return FAVORITE_INVALID;
        /* Inventory Visit continues past an empty entry. */
        if (entry) {
            if (!read_word(memory, entry, 0, &item)) return FAVORITE_INVALID;
            if (item == form) {
                uint32_t instances;
                if (!read_word(memory, entry, 4, &instances)) return FAVORITE_INVALID;
                enum FavoriteResult result = instance_has_hotkey(memory, instances);
                if (result != FAVORITE_NO) return result;
            }
        }
        if (!advance(memory, &cursor)) return FAVORITE_INVALID;
    }
    return FAVORITE_NO;
}

enum FavoriteResult classic_favorite_query(const FavoriteMemory *memory, uint32_t player,
                                           uint32_t magic_favorites, uint32_t form) {
    if (!player || !form) return FAVORITE_NO;
    uint8_t type;
    if (!memory || !memory->read || !memory->extra ||
        !read_field(memory, form, FORM_TYPE, &type, sizeof(type))) return FAVORITE_INVALID;
    if (type == FORM_SPELL || type == FORM_SHOUT) return magic_contains(memory, magic_favorites, form);
    return inventory_contains(memory, player, form);
}
