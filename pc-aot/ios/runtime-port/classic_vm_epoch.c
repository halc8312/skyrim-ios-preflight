/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_vm_epoch.h"
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t epoch, reverts, failed_loads, refused_commits;
static uint32_t registry;
static bool open, started;
static _Thread_local bool committing;
static struct { SkyrimVmRetire retire; void *context; } subscribers[SKYRIM_VM_EPOCH_MAX_SUBSCRIBERS];
static unsigned subscriber_count;

/* Lock held. Every scope taken before this call becomes stale. */
static void retire_current(void) {
    const uint64_t old = epoch++;
    open = false;
    for (unsigned n = 0; n < subscriber_count; ++n) subscribers[n].retire(subscribers[n].context, old);
}

bool skyrim_vm_epoch_subscribe(SkyrimVmRetire retire, void *context) {
    bool accepted = false;
    pthread_mutex_lock(&lock);
    if (retire && !started && subscriber_count < SKYRIM_VM_EPOCH_MAX_SUBSCRIBERS) {
        subscribers[subscriber_count].retire = retire;
        subscribers[subscriber_count].context = context;
        ++subscriber_count;
        accepted = true;
    }
    pthread_mutex_unlock(&lock);
    return accepted;
}

void skyrim_vm_epoch_registered(uint32_t value) {
    if (!value) return;
    pthread_mutex_lock(&lock);
    started = true;
    if (value != registry) {
        /* A first or replaced VM. Anything taken against another registry is stale. */
        if (registry) retire_current();
        else ++epoch;
        registry = value;
        open = true;
    }
    pthread_mutex_unlock(&lock);
}

void skyrim_vm_epoch_before_revert(unsigned site) {
    (void)site;
    pthread_mutex_lock(&lock);  /* waits for a commit in progress */
    started = true;
    ++reverts;
    retire_current();
    pthread_mutex_unlock(&lock);
}

void skyrim_vm_epoch_after_load(int succeeded) {
    pthread_mutex_lock(&lock);
    if (succeeded && registry) {
        open = true;
    } else {
        open = false;
        ++failed_loads;
    }
    pthread_mutex_unlock(&lock);
}

static bool vm_scope(bool opened, SkyrimVmScope *out) {
    if (!out) return false;
    pthread_mutex_lock(&lock);
    const bool current = registry && open == opened;
    if (current) *out = (SkyrimVmScope){epoch, registry};
    pthread_mutex_unlock(&lock);
    return current;
}

bool skyrim_vm_scope_current(SkyrimVmScope *out) { return vm_scope(true, out); }
bool skyrim_vm_scope_pending_load(SkyrimVmScope *out) { return vm_scope(false, out); }

bool skyrim_vm_commit_begin(const SkyrimVmScope *scope) {
    if (!scope || committing) return false;
    pthread_mutex_lock(&lock);
    if (open && scope->epoch == epoch && scope->registry == registry) {
        committing = true;
        return true;
    }
    ++refused_commits;
    pthread_mutex_unlock(&lock);
    return false;
}

void skyrim_vm_commit_end(void) {
    if (!committing) return;
    committing = false;
    pthread_mutex_unlock(&lock);
}

void skyrim_vm_epoch_stats(SkyrimVmEpochStats *out) {
    if (!out) return;
    pthread_mutex_lock(&lock);
    *out = (SkyrimVmEpochStats){epoch, reverts, failed_loads, refused_commits, open};
    pthread_mutex_unlock(&lock);
}
