/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "PcGuestModHooksABI.h"

/* Once on the startup thread. The path is the exact profile argument the
 * runtime will receive, including its container alias if any. */
bool skyrim_save_app_configure(const char *profile);
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
bool skyrim_save_app_configure_storage(const char *profile, const uint8_t profile_id[16],
                                      unsigned text, unsigned voices);
/* Explicit Settings copy, called from a worker. Low result bit is a same-name
 * .skse and bit 1 is app-owned companion data. */
int skyrim_save_app_copy_selected(int root, const char *source, const char *target,
    const char *target_profile, const uint8_t target_id[16], const char *leaf, int *copied);
#endif
void skyrim_save_app_fill(PcGuestModHooks *);
void skyrim_save_app_retire(const struct CPU *);
