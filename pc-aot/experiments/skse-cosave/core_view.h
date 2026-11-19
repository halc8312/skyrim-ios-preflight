/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "core_snapshot.h"
#include "records.h"

struct SkseCoreSnapshot {
    SksePairImage image;
    SkseOwnerView core;
    SkseModMap map;
};

/* Decode only a core-owner view returned by skse_records_owner for a
 * verified image. Callers own the surrounding image for the entire call. */
SkseModMapResult skse_mod_map_decode_core(const SkseOwnerView *core, const SkseLoadedMod *current,
                                         size_t count, SkseModMap *out);
