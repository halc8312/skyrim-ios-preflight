/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "classic_vm_epoch.h"

typedef struct {
    SkyrimVmScope scope;
    uint64_t handle;
    uint32_t actor, magnitude;
    bool keep;
} SkyrimHighHeelUpdate;

/* A consumer copies a scalar request into its queue and returns whether
 * it was accepted. The callback runs with the VM and lifetime locks held. */
typedef bool (*SkyrimHighHeelQueueUpdate)(void *, const SkyrimHighHeelUpdate *);

/* Install before guest startup only when a scene consumer is ready. */
bool skyrim_highheel_native_prepare(SkyrimHighHeelQueueUpdate, void *);
bool skyrim_highheel_native_retire(void);
