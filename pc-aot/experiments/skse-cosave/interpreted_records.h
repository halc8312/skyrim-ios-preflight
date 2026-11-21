/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "event_bridge.h"
#include "plugin_trial.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Copy the completed callback's immutable records into the pending save.
 * The stage retains all other Classic owners from its verified base. */
SksePairResult skse_bridge_write_interpreted(SksePendingSave *pending, const SksePluginRecordSet *records);

typedef struct {
    const SkseLoadedMod *current;
    size_t current_count;
    SkseModMap map;
    SkseModMapResult status;
} SkseInterpretedModMap;

/* The output must start NULL. After argument validation, this consumes the
 * load input as finish_load_input does. A verified image with no entry for
 * this UID gives the caller an empty reader. A rejected or missing image
 * gives no records. A requested mapping is cleared first and populated only
 * from UID 0 of the same verified image. Map errors do not discard raw records.
 * This function never invokes the DLL or changes its VM. */
SkseBridgeResult skse_bridge_finish_interpreted_load(SkseLoadInput **pending,
    const SkseEventTrace *trace, const SkseBaseEvidence *completed, uint32_t uid,
    SksePluginRecordSet **out, SkseInterpretedModMap *catalog, SksePairResult *detail);

/* Decode an already verified image returned by the native-load bridge. The
 * record set owns its own data; the caller still owns the source image. */
SksePairResult skse_interpreted_decode(const void *bytes, size_t size, uint32_t uid,
    SksePluginRecordSet **out, SkseInterpretedModMap *catalog);

/* The returned callbacks borrow the map only during the interpreted load. */
SkseSerialResolvers skse_interpreted_resolvers(const SkseModMap *map);

#ifdef __cplusplus
}
#endif
