/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "mod_racemenu_preset.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t profile_generation;
    uint64_t menu_generation;
    uint32_t menu;
    uint32_t actor;
    uint32_t base;
} SkyrimRaceMenuScope;

typedef struct {
    SkyrimRaceMenuScope scope;
    float value;
    bool pending;
} SkyrimRaceMenuNoseRequest;

/* The menu owner calls both functions. The runtime supplies fresh generations
 * on profile changes and menu recreation, including reuse of an old address. */
bool skyrim_racemenu_queue_nose_height(SkyrimRaceMenuNoseRequest *request,
                                      const SkyrimRaceMenuScope *scope,
                                      const SkyrimRaceMenuPreset *preset);
bool skyrim_racemenu_take_nose_height(SkyrimRaceMenuNoseRequest *request,
                                     const SkyrimRaceMenuScope *current,
                                     float minimum, float maximum, float *value);
