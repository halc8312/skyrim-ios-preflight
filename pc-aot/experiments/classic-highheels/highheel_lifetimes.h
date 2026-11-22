/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "highheel_effect.h"

typedef struct {
    uint64_t handle;
    uint32_t magnitude;
    bool started, finished, reused;
} HighHeelLifetime;

typedef struct {
    uint64_t epoch;
    uint32_t registry;
    size_t capacity, count;
    HighHeelLifetime *slots;
    bool incomplete;
} HighHeelLifetimes;

/* Classic's stock effect listener encodes actor FormID in the low word and
 * the nonzero effect number with flag 0x20000 in the high word. */
uint64_t highheel_effect_handle(uint32_t actor, uint32_t number);

/* Observe only a sample authenticated at the retained stock publication.
 * The caller serializes access and verifies the scope against the live VM. */
void highheel_lifetimes_observe(HighHeelLifetimes *, uint64_t epoch, uint32_t registry,
                                const HighHeelEffectSample *);

/* Confirm the typed self's handle and scalar argument against a published
 * lifetime. Stop may follow a sit (active) or the original finish dispatch. */
bool highheel_lifetimes_match(const HighHeelLifetimes *, uint64_t epoch, uint32_t registry,
                              uint64_t handle, uint32_t actor, bool keep, uint32_t magnitude);
void highheel_lifetimes_clear(HighHeelLifetimes *);
