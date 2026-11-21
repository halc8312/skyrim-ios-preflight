/* SPDX-License-Identifier: GPL-3.0-only
 * Classic Papyrus VM generation. A native call that finishes after the VM was
 * reverted (load or script reload) must not write its result or its files.
 * The guest VM_LIFECYCLE hooks report the revert before the original revert
 * runs and the load result after the original VM load returns; Classic's
 * registration hook reports the registry.
 *
 * Lock order: this lock, then any lock a subscriber or a committer takes.
 * Subscribers run with this lock held and must not call back into this API.
 * While a commit holds the lock the committer may copy a result or apply
 * already validated, bounded private records. A revert waits for it to end.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SKYRIM_VM_EPOCH_MAX_SUBSCRIBERS = 8 };
enum { SKYRIM_VM_REVERT_LOAD = 1, SKYRIM_VM_REVERT_SCRIPT_RELOAD = 2 };

typedef struct { uint64_t epoch; uint32_t registry; } SkyrimVmScope;
typedef void (*SkyrimVmRetire)(void *context, uint64_t old_epoch);

void skyrim_vm_epoch_registered(uint32_t registry);  /* after Classic's registration hook */
void skyrim_vm_epoch_before_revert(unsigned site);   /* immediately before the original revert */
void skyrim_vm_epoch_after_load(int succeeded);      /* immediately after the original VM load */
bool skyrim_vm_scope_current(SkyrimVmScope *out);    /* false while reverting, closed or unregistered */
/* Same registered VM after a revert, before a reviewed original load.
 * It remains invalid for commit until the engine marks that generation open. */
bool skyrim_vm_scope_pending_load(SkyrimVmScope *out);
bool skyrim_vm_commit_begin(const SkyrimVmScope *);  /* true: lock held and the scope is current */
void skyrim_vm_commit_end(void);                     /* only after a successful commit_begin */
/* Register before the first skyrim_vm_epoch_registered call (before guest start). */
bool skyrim_vm_epoch_subscribe(SkyrimVmRetire, void *context);

typedef struct { uint64_t epoch, reverts, failed_loads, refused_commits; bool open; } SkyrimVmEpochStats;
void skyrim_vm_epoch_stats(SkyrimVmEpochStats *out);

#ifdef __cplusplus
}
#endif
