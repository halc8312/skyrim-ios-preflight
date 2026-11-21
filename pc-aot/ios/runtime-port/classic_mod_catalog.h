/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "mod_history.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClassicModCatalog ClassicModCatalog;
typedef bool (*ClassicModCatalogRead)(void *context, uint32_t address, void *bytes, size_t length);

typedef enum {
    CLASSIC_MOD_CATALOG_OK,
    CLASSIC_MOD_CATALOG_ARGUMENT,
    CLASSIC_MOD_CATALOG_UNAVAILABLE,
    CLASSIC_MOD_CATALOG_INVALID,
    CLASSIC_MOD_CATALOG_CHANGED,
    CLASSIC_MOD_CATALOG_NO_MEMORY
} ClassicModCatalogResult;

/* Call at an authenticated, quiescent LE 1.9.32 DataHandler boundary. The
 * reader must keep live objects readable for the duration and make bounded
 * copies without changing guest state. Rechecks reject observable changes.
 * Supply a NULL *out. Only a completed snapshot is published there. */
ClassicModCatalogResult classic_mod_catalog_capture(ClassicModCatalogRead read, void *context, ClassicModCatalog **out);
/* Rows and names are copied and remain valid until free. Rows are in actual
 * load index order and usable by the Classic co-save remapper. */
const SkseLoadedMod *classic_mod_catalog_entries(const ClassicModCatalog *, size_t *count);
void classic_mod_catalog_free(ClassicModCatalog *);

#ifdef __cplusplus
}
#endif
