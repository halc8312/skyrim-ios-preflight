/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "mod_history.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SkseCoreSnapshot SkseCoreSnapshot;
typedef struct { const uint8_t *data; size_t size; } SkseCoreName;
typedef enum {
    SKSE_CORE_OK, SKSE_CORE_ARGUMENT, SKSE_CORE_SOURCE_ERROR, SKSE_CORE_MOD_HISTORY_ERROR
} SkseCoreSnapshotResult;
/* Retain one verified companion and its resolved mod map for all read-only
 * core inspectors during this load. Close on revert or change of context. */
SkseCoreSnapshotResult skse_core_snapshot_open(SksePairStore *store, const SksePairGeneration *generation,
    const SkseLoadedMod *current, size_t count, SkseCoreSnapshot **out, SksePairResult *source_result,
    SkseModMapResult *map_result);
void skse_core_snapshot_close(SkseCoreSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
