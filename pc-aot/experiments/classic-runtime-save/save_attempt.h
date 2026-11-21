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

/* The caller has authenticated the owner and spilled frame and has resolved
 * the temporary path with the real file mapper. This call authorizes that
 * path against the selected profile directory. Keep the state and directory
 * on this owner thread until ordinary return or explicit cancellation. */
uint64_t classic_save_attempt_begin(ClassicSaveAttempt *, const struct CPU *, int directory,
                                    const char *mapped_directory, const char *mapped_temporary,
                                    ClassicSaveAttemptReport, void *context);

/* Admit the actual factory call only on its runtime owner thread and with
 * the reviewed spilled frame. Maps the original NUL-terminated path through
 * the same mapper as the native CreateFile shim. */
uint64_t classic_save_attempt_begin_guest(ClassicSaveAttempt *, const struct CPU *, uint32_t stack,
    uint32_t temporary_path, int directory, const char *mapped_directory, ClassicSaveAttemptReport, void *context);
uint64_t classic_save_attempt_current(const struct CPU *, unsigned phase);
void classic_save_attempt_event(const struct CPU *, uint64_t ticket, unsigned site, int succeeded);
void classic_save_attempt_cancel(const struct CPU *, uint64_t ticket);
