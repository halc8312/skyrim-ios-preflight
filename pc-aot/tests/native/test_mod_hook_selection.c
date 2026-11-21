/* Which hook groups a launch selects for its features. */
#include "mod_hook_selection.h"
#include <assert.h>

enum { MENU = 1, WORLD = 2, VM = 4, CHARGEN = 8, SLIDER = 32 };
#define BIT(f) (1u << (f))

int main(void) {
    SkyrimHookPlan plan;
    assert(skyrim_mod_hook_requirements(SKYRIM_FEATURE_FUZ) == (MENU | WORLD | VM));
    assert(skyrim_mod_hook_requirements(SKYRIM_FEATURE_PAPYRUSUTIL) == VM);
    assert(skyrim_mod_hook_requirements(SKYRIM_FEATURE_PRECACHE_KILLER) == CHARGEN);
    assert(skyrim_mod_hook_requirements(SKYRIM_FEATURE_RACEMENU) == SLIDER);
    assert(!skyrim_mod_hook_requirements(99));

    /* Nothing requested: nothing selected. */
    skyrim_mod_hook_plan(0, 15, 15, &plan);
    assert(!plan.groups && !plan.enabled && plan.reason[SKYRIM_FEATURE_FUZ] == SKYRIM_HOOK_PLAN_NOT_REQUESTED);

    /* Fuz with every group: selected with the VM generation. */
    skyrim_mod_hook_plan(BIT(SKYRIM_FEATURE_FUZ), MENU | WORLD | VM, MENU | WORLD | VM, &plan);
    assert(plan.groups == (MENU | WORLD | VM) && plan.enabled == BIT(SKYRIM_FEATURE_FUZ));

    /* A guest without the VM group: Fuz stays off. */
    skyrim_mod_hook_plan(BIT(SKYRIM_FEATURE_FUZ), MENU | WORLD, MENU | WORLD | VM, &plan);
    assert(!plan.groups && !plan.enabled);
    assert(plan.reason[SKYRIM_FEATURE_FUZ] == SKYRIM_HOOK_PLAN_GUEST_HOOKS_MISSING && plan.missing[SKYRIM_FEATURE_FUZ] == VM);

    /* The app cannot provide a group: host reason, even when the guest has it. */
    skyrim_mod_hook_plan(BIT(SKYRIM_FEATURE_PRECACHE_KILLER) | BIT(SKYRIM_FEATURE_PAPYRUSUTIL), 15,
                         MENU | WORLD | VM, &plan);
    assert(plan.reason[SKYRIM_FEATURE_PRECACHE_KILLER] == SKYRIM_HOOK_PLAN_HOST_UNAVAILABLE);
    assert(plan.groups == VM && plan.enabled == BIT(SKYRIM_FEATURE_PAPYRUSUTIL));

    /* Failed preparation removes only Fuz's groups, not the shared VM group. */
    skyrim_mod_hook_plan(BIT(SKYRIM_FEATURE_FUZ) | BIT(SKYRIM_FEATURE_PAPYRUSUTIL),
                         MENU | WORLD | VM, VM, &plan);
    assert(plan.reason[SKYRIM_FEATURE_FUZ] == SKYRIM_HOOK_PLAN_HOST_UNAVAILABLE);
    assert(plan.groups == VM && plan.enabled == BIT(SKYRIM_FEATURE_PAPYRUSUTIL));

    /* The slider is available only with its stage and host callbacks. */
    skyrim_mod_hook_plan(BIT(SKYRIM_FEATURE_RACEMENU), 15, SLIDER, &plan);
    assert(!plan.groups && plan.reason[SKYRIM_FEATURE_RACEMENU] == SKYRIM_HOOK_PLAN_GUEST_HOOKS_MISSING);
    skyrim_mod_hook_plan(BIT(SKYRIM_FEATURE_RACEMENU), SLIDER, SLIDER, &plan);
    assert(plan.groups == SLIDER && plan.enabled == BIT(SKYRIM_FEATURE_RACEMENU));
    return 0;
}
