/* SPDX-License-Identifier: GPL-3.0-only
 * App implementation of the FUZ_MENU and FUZ_WORLD guest hook groups. It
 * links the reviewed experiments/fuz-voice code unchanged: the voice policy,
 * the menu and world trackers and the game resource lookup.
 */
#pragma once
#include "PcGuestModHooksABI.h"
#include "mod_fuz.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime layout (vendor runtime rt.h and kernel32.c rt_thread_init). */
enum {
    SKYRIM_FUZ_GUEST_LOW_GUARD = 0x10000u,   /* rt_host_reserve_guest leaves it inaccessible */
    SKYRIM_FUZ_STACK_BASE = 0x10000000u, SKYRIM_FUZ_STACK_SIZE = 0x00400000u, SKYRIM_FUZ_STACK_SLOTS = 30,
    SKYRIM_FUZ_STACK_TOP_RESERVE = 64,       /* stack_hi = stack_lo + size - 64 */
    SKYRIM_FUZ_TEB_BASE = 0x18000000u, SKYRIM_FUZ_TEB_STRIDE = 0x2000u,
    SKYRIM_FUZ_NESTED_BYTES = 64u * 1024u    /* first estimate; measured with the pinned lift (plan 8.2) */
};

typedef struct {
    uint64_t substituted, kept, missing, errors;
} SkyrimFuzHostCounts;

/* Call once on the startup thread before the guest starts. The policy uses
 * the profile's settings and the ten stock clip names of mod_fuz.h. */
int skyrim_fuz_app_configure(const SkyrimFuzSettings *settings);
/* Clear both trackers; registered with skyrim_vm_epoch_subscribe. */
void skyrim_fuz_app_retire(void *context, uint64_t old_epoch);
void skyrim_fuz_app_fill(PcGuestModHooks *table); /* sets fuz_menu and fuz_world */
void skyrim_fuz_app_counts(SkyrimFuzHostCounts *out);
/* Print IOS_FUZ. Not final: at most once a minute, from dialogue hooks only. */
void skyrim_fuz_app_report(int final);
/* The stack interval of the guest thread owning this TEB (fs_base), or false. */
bool skyrim_fuz_app_stack(uint32_t fs_base, uint32_t *limit, uint32_t *base);

#ifdef __cplusplus
}
#endif
