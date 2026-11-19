/* SPDX-License-Identifier: GPL-3.0-only */
#include "core_view.h"
#include <stdlib.h>

SkseCoreSnapshotResult skse_core_snapshot_open(SksePairStore *store, const SksePairGeneration *generation,
    const SkseLoadedMod *current, size_t count, SkseCoreSnapshot **out, SksePairResult *source_result,
    SkseModMapResult *map_result) {
    if (source_result) *source_result = SKSE_PAIR_ARGUMENT;
    if (map_result) *map_result = SKSE_MAP_ARGUMENT;
    if (!out) return SKSE_CORE_ARGUMENT;
    *out = NULL;
    if (!store || !generation) return SKSE_CORE_ARGUMENT;
    SkseCoreSnapshot *view = calloc(1, sizeof *view);
    void *bytes = NULL;
    SksePairResult source = view ? skse_pair_export(store, generation, &bytes, &view->image.size) : SKSE_PAIR_STORAGE;
    if (source == SKSE_PAIR_OK) {
        view->image.data = bytes;
        source = skse_records_owner(&view->image, 0, &view->core);
    }
    if (source_result) *source_result = source;
    SkseModMapResult mapping = source == SKSE_PAIR_OK
        ? skse_mod_map_decode_core(&view->core, current, count, &view->map) : SKSE_MAP_SOURCE_ERROR;
    if (map_result) *map_result = mapping;
    if (source != SKSE_PAIR_OK || mapping != SKSE_MAP_OK) {
        skse_core_snapshot_close(view);
        return source == SKSE_PAIR_OK ? SKSE_CORE_MOD_HISTORY_ERROR : SKSE_CORE_SOURCE_ERROR;
    }
    *out = view;
    return SKSE_CORE_OK;
}

void skse_core_snapshot_close(SkseCoreSnapshot *view) {
    if (view) { free(view->image.data); free(view); }
}
