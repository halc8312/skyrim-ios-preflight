/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "core_snapshot.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKSE_REG_MENU_EVENTS, SKSE_REG_MOD_CALLBACKS, SKSE_REG_INPUT_KEYS, SKSE_REG_INPUT_CONTROLS,
    SKSE_REG_ACTOR_ACTIONS, SKSE_REG_CROSSHAIR, SKSE_REG_CAMERA, SKSE_REG_NODE_WRITER, SKSE_REG_NODE_READER
} SkseRegistrationFamily;
typedef enum { SKSE_REG_NO_KEY, SKSE_REG_NAMED_KEY, SKSE_REG_NUMERIC_KEY } SkseRegistrationKeyType;
typedef struct {
    uint32_t tag;
    SkseRegistrationKeyType key_type;
    int named_callback, recognized_by_classic_reader;
} SkseRegistrationFormat;
int skse_registration_format(SkseRegistrationFamily family, SkseRegistrationFormat *out);
typedef enum {
    SKSE_REG_OK = SKSE_CORE_OK, SKSE_REG_ARGUMENT = SKSE_CORE_ARGUMENT,
    SKSE_REG_SOURCE_ERROR = SKSE_CORE_SOURCE_ERROR, SKSE_REG_MOD_HISTORY_ERROR = SKSE_CORE_MOD_HISTORY_ERROR,
    SKSE_REG_MISSING, SKSE_REG_UNSUPPORTED, SKSE_REG_INVALID, SKSE_REG_DUPLICATE,
    SKSE_REG_OUT_OF_ORDER, SKSE_REG_LIMIT
} SkseRegistrationResult;
enum { SKSE_REG_MAX_GROUPS = 4096, SKSE_REG_MAX_HANDLES = 65536 };
typedef struct {
    uint32_t groups, handles;
    uint32_t resolutions[SKSE_MOD_ARGUMENT + 1];
} SkseRegistrationSummary;
typedef SkseCoreName SkseRegistrationName;
typedef struct { SkseRegistrationKeyType type; SkseRegistrationName name; uint32_t number; } SkseRegistrationKey;
typedef void (*SkseRegistrationObserver)(void *ctx, SkseRegistrationKey event, SkseRegistrationName callback,
                                         uint64_t saved, SkseModResolution resolution, const uint64_t *mapped);

typedef SkseCoreSnapshot SkseRegistrationView;
/* One immutable view verifies the base file and builds the mod map once per
 * accepted generation. Further families reuse those bytes without file I/O.
 * Release the view on revert, generation change or profile switch. */
SkseRegistrationResult skse_registration_view_open(SksePairStore *store, const SksePairGeneration *generation,
    const SkseLoadedMod *current, size_t count, SkseRegistrationView **out, SksePairResult *source_result,
    SkseModMapResult *map_result);
void skse_registration_view_close(SkseRegistrationView *view);
SkseRegistrationResult skse_registration_view_read(const SkseRegistrationView *view, SkseRegistrationFamily family,
    SkseRegistrationSummary *summary, SkseRegistrationObserver observer, void *context);

/* Read only the selected family of UID 0 in a verified generation. Observers
 * run after full validation, with borrowed byte strings and a numeric handle.
 * Only ModEvent rows have a callback name. For a keyless set, groups counts
 * its single top-level record, even when empty. Partial/invalid input yields no rows. */
SkseRegistrationResult skse_registration_history_load(SksePairStore *store, const SksePairGeneration *generation,
    const SkseLoadedMod *current, size_t count, SkseRegistrationFamily family, SkseRegistrationSummary *summary,
    SkseRegistrationObserver observer, void *context, SksePairResult *source_result, SkseModMapResult *map_result);
#ifdef __cplusplus
}
#endif
