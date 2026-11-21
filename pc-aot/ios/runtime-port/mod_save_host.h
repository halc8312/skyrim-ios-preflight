/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "PcGuestModHooksABI.h"

/* Once on the startup thread. The path is the exact profile argument the
 * runtime will receive, including its container alias if any. */
bool skyrim_save_app_configure(const char *profile);
void skyrim_save_app_fill(PcGuestModHooks *);
void skyrim_save_app_retire(const struct CPU *);
