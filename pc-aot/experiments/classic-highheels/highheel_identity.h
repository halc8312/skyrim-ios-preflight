/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "classic_mod_catalog.h"

typedef enum {
    HIGHHEEL_IDENTITY_MATCH,
    HIGHHEEL_IDENTITY_NOT_LOADED,
    HIGHHEEL_IDENTITY_ASSET_DIFFERENT,
    HIGHHEEL_IDENTITY_UNREADABLE,
    HIGHHEEL_IDENTITY_POSSIBLE_OVERRIDE
} HighHeelIdentityResult;

typedef bool (*HighHeelDigest)(const void *bytes, size_t length, uint8_t sha256[32]);

/* Verify the released loose files in the selected Data view. This does not
 * establish the game's actual load index, effective record, or VM binding. */
HighHeelIdentityResult highheel_identity_check_assets(int data, HighHeelDigest digest);

/* Join a stable deployed Data directory and a copied catalogue from the same
 * launch. Check the exact owner ESM and loose PEX, then reject later loaded
 * plugins that could override the owner record. Only MATCH writes setting_id.
 * A caller must separately authenticate the VM's actual PEX binding and the
 * actor/effect borrow before acting on a native event. */
HighHeelIdentityResult highheel_identity_join(int data, const ClassicModCatalog *catalog,
                                              HighHeelDigest digest, uint32_t *setting_id);
