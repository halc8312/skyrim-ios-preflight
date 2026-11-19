/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "generation.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { SKSE_CLASSIC_MOD_LIMIT = 255, SKSE_CLASSIC_MOD_NAME_LIMIT = 259 };
typedef enum { SKSE_LOADED_CLASSIC, SKSE_LOADED_INCOMPATIBLE } SkseLoadedModKind;
typedef struct {
    const uint8_t *name;
    size_t length;
    /* Actual guest DataHandler index. Ignored for an explicitly incompatible candidate. */
    uint8_t index;
    SkseLoadedModKind kind;
} SkseLoadedMod;

typedef enum {
    SKSE_MAP_OK, SKSE_MAP_ARGUMENT, SKSE_MAP_SOURCE_ERROR, SKSE_MAP_HISTORY_MISSING,
    SKSE_MAP_UNSUPPORTED_VERSION, SKSE_MAP_INVALID_HISTORY, SKSE_MAP_DUPLICATE_HISTORY,
    SKSE_MAP_DUPLICATE_SAVED_NAME, SKSE_MAP_INVALID_CURRENT
} SkseModMapResult;
typedef enum {
    SKSE_MOD_RESOLVED, SKSE_MOD_DYNAMIC, SKSE_MOD_NOT_READY, SKSE_MOD_INDEX_NOT_SAVED,
    SKSE_MOD_MISSING_OR_RENAMED, SKSE_MOD_INCOMPATIBLE, SKSE_MOD_AMBIGUOUS, SKSE_MOD_ARGUMENT
} SkseModResolution;
typedef struct {
    uint8_t saved_count, ready;
    uint8_t current_index[SKSE_CLASSIC_MOD_LIMIT], state[SKSE_CLASSIC_MOD_LIMIT];
} SkseModMap;

void skse_mod_map_clear(SkseModMap *map);
/* Build from UID 0 of a verified generation. The map is cleared on every failure.
 * Optional source_result gives the exact pairing or Classic file failure.
 * The supplied names and the MODS payload must use the same guest byte encoding.
 * A successful map can contain unresolved individual entries. */
SkseModMapResult skse_mod_map_load(SksePairStore *store, const SksePairGeneration *generation,
                                  const SkseLoadedMod *current, size_t count, SkseModMap *out,
                                  SksePairResult *source_result);
/* Only RESOLVED and DYNAMIC write the output. DYNAMIC (mod byte 0xff) passes
 * through even with a cleared map, matching the Classic interface. */
SkseModResolution skse_mod_resolve_form(const SkseModMap *map, uint32_t saved, uint32_t *out);
SkseModResolution skse_mod_resolve_handle(const SkseModMap *map, uint64_t saved, uint64_t *out);

#ifdef __cplusplus
}
#endif
