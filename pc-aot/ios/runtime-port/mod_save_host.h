/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "PcGuestModHooksABI.h"

/* Once on the startup thread. The path is the exact profile argument the
 * runtime will receive, including its container alias if any. */
bool skyrim_save_app_configure(const char *profile);
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
bool skyrim_save_app_configure_storage(const char *profile, const uint8_t profile_id[16],
                                      unsigned text, unsigned voices);
#endif
void skyrim_save_app_fill(PcGuestModHooks *);
void skyrim_save_app_retire(const struct CPU *);
