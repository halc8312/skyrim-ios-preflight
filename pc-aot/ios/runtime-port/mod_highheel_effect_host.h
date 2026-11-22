/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "PcGuestModHooksABI.h"
#include "highheel_identity.h"

/* Prepare once before guest startup. Holds a duplicate of the readonly
 * launch Data descriptor until the startup callback or cancellation. */
bool skyrim_highheel_effect_prepare(int data, HighHeelDigest digest);
void skyrim_highheel_effect_cancel(void);
void skyrim_highheel_effect_fill(PcGuestModHooks *);
