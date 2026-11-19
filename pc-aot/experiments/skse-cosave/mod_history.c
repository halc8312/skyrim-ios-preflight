/* SPDX-License-Identifier: GPL-3.0-only */
#include "core_view.h"
#include <stdlib.h>
#include <string.h>

/* The Classic multicharacter constant 'MODS' is a uint32_t on the wire. */
#define CLASSIC_MODS UINT32_C(0x4d4f4453)
typedef struct { const uint8_t *bytes; size_t size; } ModName;
typedef struct { ModName name; uint8_t index, compatible; } Candidate;
typedef struct {
    Candidate candidates[SKSE_CLASSIC_MOD_LIMIT];
    size_t count;
    SkseModMap table;
    SkseModMapResult result;
    int seen;
} History;

static uint8_t fold(uint8_t value) {
    return value >= 'A' && value <= 'Z' ? (uint8_t)(value + ('a' - 'A')) : value;
}
static int compare_names(ModName a, ModName b) {
    size_t n = a.size < b.size ? a.size : b.size;
    for (size_t i = 0; i < n; ++i) {
        int diff = (int)fold(a.bytes[i]) - (int)fold(b.bytes[i]);
        if (diff) return diff;
    }
    return (a.size > b.size) - (a.size < b.size);
}
static int order_name(const void *a, const void *b) {
    return compare_names(*(const ModName *)a, *(const ModName *)b);
}
static int order_candidate(const void *a, const void *b) {
    return compare_names(((const Candidate *)a)->name, ((const Candidate *)b)->name);
}
static int valid_name(ModName name) {
    return name.bytes && name.size && name.size <= SKSE_CLASSIC_MOD_NAME_LIMIT &&
           !memchr(name.bytes, 0, name.size);
}
static SkseModMapResult prepare_current(History *history, const SkseLoadedMod *current, size_t count) {
    if (count > SKSE_CLASSIC_MOD_LIMIT || (count && !current)) return SKSE_MAP_INVALID_CURRENT;
    uint8_t used[SKSE_CLASSIC_MOD_LIMIT] = {0};
    for (size_t i = 0; i < count; ++i) {
        const SkseLoadedMod *entry = &current[i];
        ModName name = {entry->name, entry->length};
        if (!valid_name(name) || (entry->kind != SKSE_LOADED_CLASSIC && entry->kind != SKSE_LOADED_INCOMPATIBLE))
            return SKSE_MAP_INVALID_CURRENT;
        if (entry->kind == SKSE_LOADED_CLASSIC) {
            if (entry->index == 255 || used[entry->index]) return SKSE_MAP_INVALID_CURRENT;
            used[entry->index] = 1;
        }
        history->candidates[i] = (Candidate){name, entry->index, entry->kind == SKSE_LOADED_CLASSIC};
    }
    history->count = count;
    qsort(history->candidates, count, sizeof history->candidates[0], order_candidate);
    return SKSE_MAP_OK;
}

static SkseModResolution find_current(const History *history, ModName name, uint8_t *index) {
    size_t lo = 0, hi = history->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare_names(history->candidates[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == history->count || compare_names(history->candidates[lo].name, name)) return SKSE_MOD_MISSING_OR_RENAMED;
    if (lo + 1 < history->count && !compare_names(history->candidates[lo + 1].name, name))
        return SKSE_MOD_AMBIGUOUS;
    if (!history->candidates[lo].compatible) return SKSE_MOD_INCOMPATIBLE;
    *index = history->candidates[lo].index;
    return SKSE_MOD_RESOLVED;
}
static SkseModMapResult parse_history(History *history, const SksePairRecord *record) {
    if (record->version != 0) return SKSE_MAP_UNSUPPORTED_VERSION;
    if (!record->length) return SKSE_MAP_INVALID_HISTORY;
    const uint8_t *data = record->data;
    const uint8_t count = data[0];
    size_t pos = 1;
    ModName saved[SKSE_CLASSIC_MOD_LIMIT];
    for (unsigned i = 0; i < count; ++i) {
        if (record->length - pos < 2) return SKSE_MAP_INVALID_HISTORY;
        size_t n = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
        pos += 2;
        if (n > record->length - pos) return SKSE_MAP_INVALID_HISTORY;
        saved[i] = (ModName){data + pos, n};
        if (!valid_name(saved[i])) return SKSE_MAP_INVALID_HISTORY;
        history->table.state[i] = (uint8_t)find_current(history, saved[i], &history->table.current_index[i]);
        pos += n;
    }
    if (pos != record->length) return SKSE_MAP_INVALID_HISTORY;
    qsort(saved, count, sizeof saved[0], order_name);
    for (unsigned i = 1; i < count; ++i)
        if (!compare_names(saved[i - 1], saved[i])) return SKSE_MAP_DUPLICATE_SAVED_NAME;
    history->table.saved_count = count;
    return SKSE_MAP_OK;
}
void skse_mod_map_clear(SkseModMap *map) {
    if (map) memset(map, 0, sizeof *map);
}
static void reset_history(void *context) {
    History *history = context;
    skse_mod_map_clear(&history->table);
    history->result = SKSE_MAP_HISTORY_MISSING;
    history->seen = 0;
}
static int visit_core(void *context, const SksePairRecord *record) {
    History *history = context;
    if (record->type == CLASSIC_MODS) {
        history->result = history->seen ? SKSE_MAP_DUPLICATE_HISTORY : parse_history(history, record);
        history->seen = 1;
    }
    return 1;
}
static SkseModMapResult finish(History *history, SkseModMap *out) {
    if (history->result == SKSE_MAP_OK) {
        history->table.ready = 1;
        *out = history->table;
    }
    return history->result;
}
SkseModMapResult skse_mod_map_decode_core(const SkseOwnerView *core, const SkseLoadedMod *current,
                                         size_t count, SkseModMap *out) {
    skse_mod_map_clear(out);
    History history;
    SkseModMapResult result = prepare_current(&history, current, count);
    if (result != SKSE_MAP_OK) return result;
    reset_history(&history);
    skse_records_each(core, visit_core, &history);
    return finish(&history, out);
}

SkseModMapResult skse_mod_map_load(SksePairStore *store, const SksePairGeneration *generation,
                                  const SkseLoadedMod *current, size_t count, SkseModMap *out,
                                  SksePairResult *source_result) {
    if (source_result) *source_result = SKSE_PAIR_ARGUMENT;
    skse_mod_map_clear(out);
    if (!out || !store || !generation) return SKSE_MAP_ARGUMENT;
    History history;
    SkseModMapResult ready = prepare_current(&history, current, count);
    if (ready != SKSE_MAP_OK) return ready;
    SksePairResult source = skse_pair_restore(store, generation, 0, reset_history, visit_core, &history);
    if (source_result) *source_result = source;
    if (source != SKSE_PAIR_OK) return SKSE_MAP_SOURCE_ERROR;
    return finish(&history, out);
}

static SkseModResolution resolve(const SkseModMap *map, uint8_t saved, uint8_t *current) {
    if (saved == 255) { *current = saved; return SKSE_MOD_DYNAMIC; }
    if (!map->ready) return SKSE_MOD_NOT_READY;
    if (saved >= map->saved_count) return SKSE_MOD_INDEX_NOT_SAVED;
    SkseModResolution result = (SkseModResolution)map->state[saved];
    if (result == SKSE_MOD_RESOLVED) *current = map->current_index[saved];
    return result;
}
SkseModResolution skse_mod_resolve_form(const SkseModMap *map, uint32_t saved, uint32_t *out) {
    if (!map || !out) return SKSE_MOD_ARGUMENT;
    uint8_t slot;
    SkseModResolution result = resolve(map, (uint8_t)(saved >> 24), &slot);
    if (result == SKSE_MOD_RESOLVED || result == SKSE_MOD_DYNAMIC)
        *out = (saved & UINT32_C(0x00ffffff)) | ((uint32_t)slot << 24);
    return result;
}
SkseModResolution skse_mod_resolve_handle(const SkseModMap *map, uint64_t saved, uint64_t *out) {
    if (!map || !out) return SKSE_MOD_ARGUMENT;
    uint8_t slot;
    SkseModResolution result = resolve(map, (uint8_t)(saved >> 24), &slot);
    if (result == SKSE_MOD_RESOLVED || result == SKSE_MOD_DYNAMIC)
        *out = (saved & UINT64_C(0xffffffff00ffffff)) | ((uint64_t)slot << 24);
    return result;
}
