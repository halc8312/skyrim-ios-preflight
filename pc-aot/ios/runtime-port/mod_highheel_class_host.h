/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "classic_vm_epoch.h"
#include "highheel_native_frame.h"

/* Called while the original linker retains the digest-verified class. */
void skyrim_highheel_class_observe(struct CPU *, SkyrimVmScope, uint32_t linked_class);
/* Snapshot scalar identities for this scope. Validate the two native identities
 * against the registry's actual publications before accepting a Run. */
bool skyrim_highheel_class_snapshot(SkyrimVmScope, HighHeelNativeBindings *);
