/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "observer/lifecycle.h"
#include "runtime_save.h"
#include "save_notifications.h"

/* The report borrows the retained .ess and the save trace only for the call.
 * Consumers that defer work must capture an immutable view first. */
typedef void (*ClassicSaveAttemptReport)(void *, int directory, const char *leaf, int ess,
                                         const SkyrimLifecycleScope *, uint32_t slot_ticket);

typedef struct {
    ClassicRuntimeSave runtime;
    SkyrimLifecycleScope lifecycle;
    SkyrimLifecycleCall slot;
    ClassicSaveAttemptReport report;
    void *context;
    enum SkyrimSaveNotice next;
    bool vm_saved, reporting;
} ClassicSaveAttempt;

/* The caller has already authenticated the owner, spilled frame, and guest
 * path against its selected profile. Keep the state and directory on this
 * owner thread until its ordinary return or explicit cancellation. */
uint64_t classic_save_attempt_begin(ClassicSaveAttempt *, const struct CPU *, int directory,
                                    const char *mapped_directory, const char *final_leaf,
                                    ClassicSaveAttemptReport, void *context);
uint64_t classic_save_attempt_current(const struct CPU *, unsigned phase);
void classic_save_attempt_event(const struct CPU *, uint64_t ticket, unsigned site, int succeeded);
void classic_save_attempt_cancel(const struct CPU *, uint64_t ticket);
