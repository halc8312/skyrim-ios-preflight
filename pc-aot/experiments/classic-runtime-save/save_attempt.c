/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "save_attempt.h"

#include <errno.h>
#include <string.h>

static _Thread_local ClassicSaveAttempt *active;

static ClassicSaveAttempt *match(const struct CPU *cpu, uint64_t ticket) {
    return active && !active->reporting && active->runtime.owner == cpu &&
           active->runtime.token == ticket ? active : NULL;
}

uint64_t classic_save_attempt_begin(ClassicSaveAttempt *attempt, const struct CPU *cpu, int directory,
                                    const char *mapped_directory, const char *mapped_temporary,
                                    ClassicSaveAttemptReport report, void *context) {
    if (active || !attempt || !report || !mapped_directory || !mapped_temporary) return 0;
    const size_t prefix = strnlen(mapped_directory, CLASSIC_RUNTIME_PATH_BYTES);
    const size_t size = strnlen(mapped_temporary, CLASSIC_RUNTIME_PATH_BYTES);
    if (!prefix || size >= CLASSIC_RUNTIME_PATH_BYTES || size <= prefix + 5 ||
        strncmp(mapped_temporary, mapped_directory, prefix) || mapped_temporary[prefix] != '/' ||
        memcmp(mapped_temporary + size - 4, ".tmp", 4)) return 0;
    const size_t leaf_size = size - prefix - 5;
    if (leaf_size > NAME_MAX) return 0;
    char leaf[NAME_MAX + 1];
    memcpy(leaf, mapped_temporary + prefix + 1, leaf_size);
    leaf[leaf_size] = '\0';
    const int original_errno = errno;
    uint64_t ticket = 0;
    if (skyrim_lifecycle_scope_open(&attempt->lifecycle)) {
        if (classic_runtime_save_begin(&attempt->runtime, cpu, directory, mapped_directory, leaf)) {
            attempt->slot = (SkyrimLifecycleCall){0};
            attempt->report = report;
            attempt->context = context;
            attempt->next = SkyrimSaveOpenFinished;
            attempt->vm_saved = attempt->reporting = false;
            active = attempt;
            classic_runtime_save_open_scope(cpu, true);
            ticket = attempt->runtime.token;
        } else {
            skyrim_lifecycle_scope_close(&attempt->lifecycle);
        }
    }
    errno = original_errno;
    return ticket;
}

uint64_t classic_save_attempt_current(const struct CPU *cpu, unsigned phase) {
    if (!active || active->reporting || active->runtime.owner != cpu) return 0;
    const enum SkyrimSaveNotice next = active->next;
    if (phase == SkyrimSaveInSlot && (next == SkyrimSaveVmReturned || next == SkyrimSaveSlotLeft))
        return active->runtime.token;
    if (phase == SkyrimSaveInClose && ((next >= SkyrimSaveFlushReturned && next <= SkyrimSaveCrtMoveReturned) ||
                                     next == SkyrimSaveCloseLeft))
        return active->runtime.token;
    return 0;
}

static void finish(ClassicSaveAttempt *attempt, bool ordinary_return) {
    const bool complete = skyrim_lifecycle_scope_close(&attempt->lifecycle);
    if (ordinary_return && complete && attempt->next == SkyrimSaveAttemptLeft && attempt->vm_saved) {
        const int reader = classic_runtime_save_reader(&attempt->runtime);
        if (reader >= 0) {
            attempt->reporting = true;
            attempt->report(attempt->context, attempt->runtime.file.directory, attempt->runtime.file.final_leaf,
                            reader, &attempt->lifecycle, attempt->slot.ticket);
        }
    }
    classic_runtime_save_end(&attempt->runtime);
    active = NULL;
}

static void advance(ClassicSaveAttempt *attempt, const struct CPU *cpu, unsigned site, bool succeeded) {
    switch (site) {
    case SkyrimSaveOpenFinished:
        classic_runtime_save_open_scope(cpu, false);
        attempt->next = SkyrimSaveSlotEntered;
        break;
    case SkyrimSaveSlotEntered:
        attempt->slot = skyrim_lifecycle_begin(SkyrimSaveSlot);
        attempt->next = SkyrimSaveVmReturned;
        break;
    case SkyrimSaveVmReturned: {
        const SkyrimLifecycleCall vm = skyrim_lifecycle_begin(SkyrimSaveVM);
        skyrim_lifecycle_end(vm, succeeded ? SkyrimReturnedTrue : SkyrimReturnedFalse);
        attempt->vm_saved = succeeded;
        attempt->next = SkyrimSaveSlotLeft;
        break;
    }
    case SkyrimSaveSlotLeft:
        skyrim_lifecycle_end(attempt->slot, SkyrimNoResult);
        attempt->next = SkyrimSaveCloseEntered;
        break;
    case SkyrimSaveCloseEntered:
        attempt->next = SkyrimSaveFlushReturned;
        break;
    case SkyrimSaveFlushReturned:
        classic_runtime_save_flush_result(cpu, succeeded);
        attempt->next = SkyrimSaveCrtCloseReturned;
        break;
    case SkyrimSaveCrtCloseReturned:
        classic_runtime_save_crt_close_result(cpu, succeeded);
        attempt->next = SkyrimSaveMoveEntered;
        break;
    case SkyrimSaveMoveEntered:
        classic_runtime_save_final_scope(cpu);
        attempt->next = SkyrimSaveCrtMoveReturned;
        break;
    case SkyrimSaveCrtMoveReturned:
        classic_runtime_save_crt_move_result(cpu, succeeded);
        attempt->next = SkyrimSaveCloseLeft;
        break;
    case SkyrimSaveCloseLeft:
        attempt->next = SkyrimSaveAttemptLeft;
        break;
    default:
        attempt->next = 0;
        break;
    }
}

void classic_save_attempt_event(const struct CPU *cpu, uint64_t ticket, unsigned site, int succeeded) {
    ClassicSaveAttempt *attempt = match(cpu, ticket);
    if (!attempt) return;
    const int original_errno = errno;
    if (site == SkyrimSaveAttemptLeft) finish(attempt, true);
    else if (site == (unsigned)attempt->next) advance(attempt, cpu, site, succeeded != 0);
    else attempt->next = 0;
    errno = original_errno;
}

void classic_save_attempt_cancel(const struct CPU *cpu, uint64_t ticket) {
    ClassicSaveAttempt *attempt = match(cpu, ticket);
    if (!attempt) return;
    const int original_errno = errno;
    finish(attempt, false);
    errno = original_errno;
}
