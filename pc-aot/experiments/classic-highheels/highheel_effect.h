/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*HighHeelEffectRead)(void *context, uint32_t address, void *bytes, size_t size);

typedef struct {
    uint32_t actor_id, setting_id, number, magnitude;
    bool starting;
} HighHeelEffectSample;

/* Call synchronously at the reviewed Classic effect publication, while the
 * producer retains the target. The reader copies only readable guest bytes.
 * expected_setting is the verified loaded FormID, including its load index.
 * On failure out is unchanged. No guest pointer leaves the callback. */
bool highheel_effect_sample(HighHeelEffectRead read, void *context, uint32_t effect, uint32_t arguments,
                            uint32_t site, uint32_t expected_setting, HighHeelEffectSample *out);
