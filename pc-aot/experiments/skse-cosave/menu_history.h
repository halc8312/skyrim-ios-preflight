/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "registration_history.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKSE_MENU_OK = SKSE_REG_OK, SKSE_MENU_ARGUMENT = SKSE_REG_ARGUMENT,
    SKSE_MENU_SOURCE_ERROR = SKSE_REG_SOURCE_ERROR, SKSE_MENU_MOD_HISTORY_ERROR = SKSE_REG_MOD_HISTORY_ERROR,
    SKSE_MENU_MISSING = SKSE_REG_MISSING, SKSE_MENU_UNSUPPORTED = SKSE_REG_UNSUPPORTED,
    SKSE_MENU_INVALID = SKSE_REG_INVALID, SKSE_MENU_DUPLICATE = SKSE_REG_DUPLICATE,
    SKSE_MENU_OUT_OF_ORDER = SKSE_REG_OUT_OF_ORDER, SKSE_MENU_LIMIT = SKSE_REG_LIMIT
} SkseMenuResult;
enum { SKSE_MENU_MAX_GROUPS = SKSE_REG_MAX_GROUPS, SKSE_MENU_MAX_HANDLES = SKSE_REG_MAX_HANDLES };
typedef SkseRegistrationSummary SkseMenuSummary;

/* Borrowed menu bytes and numeric handle mapping for one serialized row.
 * mapped is NULL for a failed resolution. The observer must be read-only.
 * This reader does not add VM references, emit events or register handlers. */
typedef void (*SkseMenuObserver)(void *context, const uint8_t *menu, size_t menu_length,
                                 uint64_t saved, SkseModResolution resolution, const uint64_t *mapped);

/* Inspect only UID 0's MENR group. All structure and budgets are validated
 * before the first observer call. The summary counts raw saved rows,
 * including repeats, and is cleared on errors. Both optional detail outputs
 * distinguish file/identity errors from a missing or unsupported MODS list. */
SkseMenuResult skse_menu_history_load(SksePairStore *store, const SksePairGeneration *generation,
                                     const SkseLoadedMod *current, size_t count,
                                     SkseMenuSummary *out, SkseMenuObserver observer, void *context,
                                     SksePairResult *source_result, SkseModMapResult *map_result);
#ifdef __cplusplus
}
#endif
