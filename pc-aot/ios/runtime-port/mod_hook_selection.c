/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_hook_selection.h"

/* PcGuestModHooksABI.h bits, repeated so this file has no seam headers. */
enum { FUZ_MENU = 1u, FUZ_WORLD = 2u, VM_LIFECYCLE = 4u, CHARGEN_PRECACHE = 8u };

uint32_t skyrim_mod_hook_requirements(unsigned feature) {
    switch (feature) {
    /* Plan P2-2: both Fuz routes, and the VM generation to clear the trackers. */
    case SKYRIM_FEATURE_FUZ: return FUZ_MENU | FUZ_WORLD | VM_LIFECYCLE;
    /* Plan P3-2: results are written only in the generation that started the call. */
    case SKYRIM_FEATURE_PAPYRUSUTIL: return VM_LIFECYCLE;
    case SKYRIM_FEATURE_PRECACHE_KILLER: return CHARGEN_PRECACHE;
    default: return 0;
    }
}

void skyrim_mod_hook_plan(uint32_t requested, uint32_t compiled, uint32_t host, SkyrimHookPlan *out) {
    *out = (SkyrimHookPlan){0};
    for (unsigned f = 0; f < SKYRIM_FEATURE_COUNT; ++f) {
        const uint32_t need = skyrim_mod_hook_requirements(f);
        if (!(requested >> f & 1u) || !need) {
            out->reason[f] = SKYRIM_HOOK_PLAN_NOT_REQUESTED;
            continue;
        }
        if (need & ~host) {
            out->reason[f] = SKYRIM_HOOK_PLAN_HOST_UNAVAILABLE;
        } else if (need & ~compiled) {
            out->reason[f] = SKYRIM_HOOK_PLAN_GUEST_HOOKS_MISSING;
            out->missing[f] = need & ~compiled;
        } else {
            out->reason[f] = SKYRIM_HOOK_PLAN_ENABLED;
            out->enabled |= 1u << f;
            out->groups |= need;
        }
    }
}
