/* SPDX-License-Identifier: GPL-3.0-only */
#include "racemenu_apply.h"

#include <assert.h>
#include <math.h>

int main(void) {
    const SkyrimRaceMenuScope initial = {1, 2, 3, 4, 5};
    SkyrimRaceMenuPreset preset = {0};
    preset.morphs[SKYRIM_RACEMENU_NOSE_HEIGHT] = 0.75f;
    SkyrimRaceMenuNoseRequest request = {0};
    float result = 0;
    assert(!skyrim_racemenu_take_nose_height(&request, &initial, -1, 1, &result));
    assert(skyrim_racemenu_queue_nose_height(&request, &initial, &preset));
    assert(!skyrim_racemenu_queue_nose_height(&request, &initial, &preset));
    assert(skyrim_racemenu_take_nose_height(&request, &initial, -1, 1, &result));
    assert(result == 0.75f);
    assert(!skyrim_racemenu_take_nose_height(&request, &initial, -1, 1, &result));

    const SkyrimRaceMenuScope others[] = {
        {2, 2, 3, 4, 5}, {1, 3, 3, 4, 5}, {1, 2, 4, 4, 5},
        {1, 2, 3, 5, 5}, {1, 2, 3, 4, 6}
    };
    for (size_t i = 0; i < sizeof others / sizeof others[0]; ++i) {
        result = 0;
        assert(skyrim_racemenu_queue_nose_height(&request, &initial, &preset));
        assert(!skyrim_racemenu_take_nose_height(&request, &others[i], -1, 1, &result));
        assert(!request.pending && result == 0);
    }

    const float bounds[][2] = {{-1, 0.5f}, {1, -1}, {0, 0}, {NAN, 1}, {-1, INFINITY}};
    for (size_t i = 0; i < sizeof bounds / sizeof bounds[0]; ++i) {
        assert(skyrim_racemenu_queue_nose_height(&request, &initial, &preset));
        assert(!skyrim_racemenu_take_nose_height(&request, &initial, bounds[i][0], bounds[i][1], &result));
        assert(!request.pending && result == 0);
    }
    preset.morphs[SKYRIM_RACEMENU_NOSE_HEIGHT] = NAN;
    assert(!skyrim_racemenu_queue_nose_height(&request, &initial, &preset));
    preset.morphs[SKYRIM_RACEMENU_NOSE_HEIGHT] = 0.75f;
    const SkyrimRaceMenuScope missing = {0, 2, 3, 4, 5};
    assert(!skyrim_racemenu_queue_nose_height(&request, &missing, &preset));
    return 0;
}
