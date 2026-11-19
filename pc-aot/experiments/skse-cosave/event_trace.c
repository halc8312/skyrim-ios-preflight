/* SPDX-License-Identifier: GPL-3.0-only */
#include "event_bridge.h"
#include <string.h>

typedef struct {
    uint16_t parent;
    uint8_t site, closed, has_child, inside, outcome;
} TraceCall;

static int boolean_site(uint32_t site) {
    return site == SkyrimLoadSlot || site == SkyrimSaveVM || site == SkyrimLoadVM;
}

SkseBridgeResult skse_bridge_check_call(const SkseGameEvent *requested, uint32_t site,
                                       const SkseEventTrace *trace) {
    if (!requested || (site != SkyrimSaveSlot && site != SkyrimLoadSlot)) return SKSE_BRIDGE_ARGUMENT;
    if (!trace || !trace->rows || !trace->count || trace->dropped ||
        trace->count > SkyrimLifecycleCapacity) return SKSE_BRIDGE_TRACE;
    if (memcmp(trace->launch, requested->scope.launch, sizeof trace->launch)) return SKSE_BRIDGE_EVENT;
    uint32_t outer = requested->ticket;
    if (!outer || outer > trace->count) return SKSE_BRIDGE_TRACE;
    TraceCall calls[SkyrimLifecycleCapacity + 1] = {{0}};
    uint32_t vm = 0, revert = 0;
    const uint32_t vm_site = site == SkyrimSaveSlot ? SkyrimSaveVM : SkyrimLoadVM;
    for (size_t i = 0; i < trace->count; ++i) {
        const SkyrimLifecycleRow *r = &trace->rows[i];
        if (!r->ticket || r->ticket > i + 1 || r->parent >= r->ticket ||
            !r->site || r->site > SkyrimRegisterPapyrus || r->returning > 1) return SKSE_BRIDGE_TRACE;
        TraceCall *call = &calls[r->ticket], *parent = &calls[r->parent];
        if (!r->returning) {
            if (r->ticket != i + 1 || r->outcome != SkyrimNoResult ||
                (r->parent && (!parent->site || parent->closed || parent->has_child))) return SKSE_BRIDGE_TRACE;
            call->parent = (uint16_t)r->parent;
            call->site = (uint8_t)r->site;
            call->inside = r->ticket == outer || (r->parent && parent->inside);
            if (r->parent) parent->has_child = 1;
            if (!call->inside) continue;
            if (r->ticket == outer) {
                if (r->site != site || r->parent) return SKSE_BRIDGE_TRACE;
            } else if (r->site == vm_site && !vm && r->parent == outer) {
                vm = r->ticket;
            } else if (site == SkyrimLoadSlot && r->site == SkyrimRevertForLoad && !revert &&
                       (!vm || !calls[vm].closed) &&
                       (r->parent == outer || r->parent == vm)) {
                revert = r->ticket;
            } else {
                return SKSE_BRIDGE_TRACE;
            }
        } else {
            if (call->site != r->site || call->parent != r->parent || call->closed || call->has_child ||
                (boolean_site(r->site) ? (r->outcome != SkyrimReturnedFalse && r->outcome != SkyrimReturnedTrue)
                                       : r->outcome != SkyrimNoResult)) return SKSE_BRIDGE_TRACE;
            call->closed = 1;
            call->outcome = (uint8_t)r->outcome;
            if (r->parent) parent->has_child = 0;
        }
    }
    if (!calls[outer].closed || calls[outer].site != site) return SKSE_BRIDGE_TRACE;
    if (calls[outer].outcome == SkyrimReturnedFalse || (vm && calls[vm].outcome == SkyrimReturnedFalse))
        return SKSE_BRIDGE_REJECTED;
    if (!vm || !calls[vm].closed || (site == SkyrimLoadSlot && (!revert || !calls[revert].closed)))
        return SKSE_BRIDGE_TRACE;
    return SKSE_BRIDGE_OK;
}
