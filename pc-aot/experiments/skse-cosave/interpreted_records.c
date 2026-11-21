/* SPDX-License-Identifier: GPL-3.0-only */
#include "interpreted_records.h"
#include "core_view.h"
#include "records.h"
#include "x86_skse_records.h"
#include <stdlib.h>

SksePairResult skse_bridge_write_interpreted(SksePendingSave *pending, const SksePluginRecordSet *records) {
    uint32_t uid = skse_plugin_record_set_uid(records);
    uint32_t count = skse_plugin_record_set_count(records);
    if (!pending || !uid || count > SKSE_X86_RECORD_LIMIT) return SKSE_PAIR_ARGUMENT;
    SksePairRecord rows[SKSE_X86_RECORD_LIMIT];
    for (uint32_t i = 0; i < count; ++i) {
        SksePluginRecord entry;
        if (!skse_plugin_record_set_at(records, i, &entry)) return SKSE_PAIR_INVALID_DATA;
        rows[i] = (SksePairRecord){entry.type, entry.version, entry.length, entry.data};
    }
    return skse_bridge_write_owner(pending, uid, rows, count);
}

typedef struct {
    SksePluginRecord rows[SKSE_X86_RECORD_LIMIT];
    uint32_t count, bytes;
} RecordSelection;

static int keep_record(void *context, const SksePairRecord *row) {
    RecordSelection *selected = context;
    if (selected->count == SKSE_X86_RECORD_LIMIT || row->length > SKSE_X86_RECORD_BYTES - selected->bytes)
        return 0;
    selected->rows[selected->count++] = (SksePluginRecord){row->type, row->version, row->length, row->data};
    selected->bytes += row->length;
    return 1;
}

SkseBridgeResult skse_bridge_finish_interpreted_load(SkseLoadInput **pending,
    const SkseEventTrace *trace, const SkseBaseEvidence *completed, uint32_t uid,
    SksePluginRecordSet **out, SkseInterpretedModMap *catalog, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (catalog) {
        skse_mod_map_clear(&catalog->map);
        catalog->status = SKSE_MAP_SOURCE_ERROR;
    }
    if (!uid || !out || *out) return SKSE_BRIDGE_ARGUMENT;
    void *bytes = NULL;
    size_t size = 0;
    SkseBridgeResult result = skse_bridge_finish_load_input(pending, trace, completed, &bytes, &size, detail);
    if (result != SKSE_BRIDGE_OK) return result;
    const SksePairImage image = {bytes, size};
    RecordSelection selected = {0};
    SksePairResult wire = skse_records_visit(&image, uid, keep_record, &selected);
    if (wire == SKSE_PAIR_OWNER_MISSING) wire = SKSE_PAIR_OK;
    if (wire == SKSE_PAIR_STOPPED) wire = SKSE_PAIR_LIMIT;
    if (wire == SKSE_PAIR_OK) {
        SkseSessionResult copied = skse_plugin_record_set_create(uid, selected.rows, selected.count, out);
        if (copied == SKSE_SESSION_NO_MEMORY) wire = SKSE_PAIR_STORAGE;
        else if (copied != SKSE_SESSION_OK) wire = SKSE_PAIR_INVALID_DATA;
    }
    if (wire == SKSE_PAIR_OK && catalog) {
        SkseOwnerView core;
        SksePairResult found = skse_records_owner(&image, 0, &core);
        if (found == SKSE_PAIR_OK)
            catalog->status = skse_mod_map_decode_core(&core, catalog->current, catalog->current_count, &catalog->map);
        else if (found == SKSE_PAIR_OWNER_MISSING) catalog->status = SKSE_MAP_HISTORY_MISSING;
    }
    free(bytes);
    if (detail) *detail = wire;
    return wire == SKSE_PAIR_OK ? SKSE_BRIDGE_OK : SKSE_BRIDGE_PAIR;
}

static bool resolve_form_id(const void *map, uint32_t saved, uint32_t *current) {
    SkseModResolution resolved = skse_mod_resolve_form(map, saved, current);
    return resolved == SKSE_MOD_RESOLVED || resolved == SKSE_MOD_DYNAMIC;
}

static bool resolve_handle(const void *map, uint64_t saved, uint64_t *current) {
    SkseModResolution resolved = skse_mod_resolve_handle(map, saved, current);
    return resolved == SKSE_MOD_RESOLVED || resolved == SKSE_MOD_DYNAMIC;
}

SkseSerialResolvers skse_interpreted_resolvers(const SkseModMap *map) {
    return (SkseSerialResolvers){map, resolve_form_id, resolve_handle};
}
