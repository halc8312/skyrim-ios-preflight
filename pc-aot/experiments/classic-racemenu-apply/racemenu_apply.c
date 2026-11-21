/* SPDX-License-Identifier: GPL-3.0-only */
#include "racemenu_apply.h"

#include <math.h>

bool skyrim_racemenu_queue_nose_height(SkyrimRaceMenuNoseRequest *request,
                                      const SkyrimRaceMenuScope *scope,
                                      const SkyrimRaceMenuPreset *preset) {
    if (!request || request->pending || !scope || !preset ||
        !scope->profile_generation || !scope->menu_generation || !scope->menu ||
        !scope->actor || !scope->base) return false;
    const float value = preset->morphs[SKYRIM_RACEMENU_NOSE_HEIGHT];
    if (!isfinite(value)) return false;
    *request = (SkyrimRaceMenuNoseRequest){*scope, value, true};
    return true;
}

bool skyrim_racemenu_take_nose_height(SkyrimRaceMenuNoseRequest *request,
                                     const SkyrimRaceMenuScope *current,
                                     float minimum, float maximum, float *value) {
    if (!request || !request->pending || !current || !value) return false;
    request->pending = false;
    const SkyrimRaceMenuScope *queued = &request->scope;
    if (queued->profile_generation != current->profile_generation ||
        queued->menu_generation != current->menu_generation ||
        queued->menu != current->menu || queued->actor != current->actor ||
        queued->base != current->base || !isfinite(minimum) || !isfinite(maximum) ||
        minimum >= maximum || request->value < minimum || request->value > maximum)
        return false;
    *value = request->value;
    return true;
}
