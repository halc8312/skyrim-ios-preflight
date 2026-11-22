/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "PcGuestModHooksABI.h"
#include "classic_vm_epoch.h"
#include "highheel_identity.h"

/* Prepare once before guest startup. Holds a duplicate of the readonly
 * launch Data descriptor until the startup callback or cancellation. */
bool skyrim_highheel_effect_prepare(int data, HighHeelDigest digest);
void skyrim_highheel_effect_cancel(void);
void skyrim_highheel_effect_fill(PcGuestModHooks *);
/* The caller supplies a borrowed self handle and a separately typed target.
 * Recheck at the point of use when the call crosses a thread boundary. */
bool skyrim_highheel_effect_matches(SkyrimVmScope, uint64_t handle, uint32_t actor, bool keep, uint32_t magnitude);
