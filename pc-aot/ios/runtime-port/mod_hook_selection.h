/* SPDX-License-Identifier: GPL-3.0-only
 * Which guest MOD hook groups a launch selects, and why a chosen feature
 * stays off. Pure decision; mod_manager.m applies it before the guest starts.
 */
#pragma once
#include <stdint.h>
#include "mod_features.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SKYRIM_HOOK_PLAN_ENABLED = 0,
    SKYRIM_HOOK_PLAN_NOT_REQUESTED = 1,
    SKYRIM_HOOK_PLAN_GUEST_HOOKS_MISSING = 2, /* rebuild the guest with the listed groups */
    SKYRIM_HOOK_PLAN_HOST_UNAVAILABLE = 3     /* this app build has no implementation */
};

/* Guest groups (PcGuestModHooksABI.h bits) a feature needs; 0 when it needs none. */
uint32_t skyrim_mod_hook_requirements(unsigned feature);

typedef struct {
    uint32_t groups;        /* to pass to pc_guest_select_mod_hooks */
    uint32_t enabled;       /* features (bit per SkyrimFeature) whose groups are all selected */
    uint8_t reason[SKYRIM_FEATURE_COUNT];
    uint32_t missing[SKYRIM_FEATURE_COUNT]; /* groups the guest lacks, per feature */
} SkyrimHookPlan;

/* requested: features chosen and otherwise allowed for this launch, among
 * those with hook requirements. compiled: groups in the opened guest.
 * host: groups this app can implement. */
void skyrim_mod_hook_plan(uint32_t requested, uint32_t compiled, uint32_t host, SkyrimHookPlan *out);

#ifdef __cplusplus
}
#endif
