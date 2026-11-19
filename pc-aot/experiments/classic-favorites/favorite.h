/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *context;
    bool (*read)(void *context, uint32_t address, void *bytes, size_t size);
    /* A checked lookup by extra-data type. A successful miss writes zero. */
    bool (*extra)(void *context, uint32_t list, uint32_t type, uint32_t *found);
} FavoriteMemory;

enum FavoriteResult {
    FAVORITE_INVALID = -1,
    FAVORITE_NO = 0,
    FAVORITE_YES = 1,
};

/* All addresses are Classic guest addresses in the supplied memory view.
 * Binding the Papyrus Form handle and getting the current player/manager
 * are responsibilities of the caller. */
enum FavoriteResult classic_favorite_query(const FavoriteMemory *memory,
                                           uint32_t player, uint32_t magic_favorites,
                                           uint32_t form);

#ifdef __cplusplus
}
#endif
