/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINSYNC_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINSYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SKSE_WIN_SYNC_LIMIT = 16, SKSE_WIN_WAIT_TIMEOUT = 0x102 };

typedef enum {
    SKSE_WIN_EVENT_CREATE, SKSE_WIN_EVENT_SET, SKSE_WIN_EVENT_RESET,
    SKSE_WIN_WAIT_ONE, SKSE_WIN_WAIT_ONE_EX, SKSE_WIN_WAIT_MANY, SKSE_WIN_WAIT_MANY_EX,
    SKSE_WIN_SEMAPHORE_CREATE, SKSE_WIN_SEMAPHORE_RELEASE, SKSE_WIN_SYNC_OPERATION_COUNT
} SkseWinSyncOperation;

typedef enum { SKSE_WIN_AUTO_EVENT, SKSE_WIN_MANUAL_EVENT, SKSE_WIN_SEMAPHORE } SkseWinSyncKind;

typedef struct {
    uint32_t token, count, maximum;
    SkseWinSyncKind kind;
} SkseWinWaitable;

typedef struct {
    uint32_t next;
    SkseWinWaitable objects[SKSE_WIN_SYNC_LIMIT];
} SkseWinSync;

uint32_t skse_win_event_create(SkseWinSync *sync, bool manual, bool signaled);
bool skse_win_event_signal(SkseWinSync *sync, uint32_t token, bool signaled);
uint32_t skse_win_semaphore_create(SkseWinSync *sync, uint32_t initial, uint32_t maximum, uint32_t *error);
bool skse_win_semaphore_release(SkseWinSync *sync, uint32_t token, uint32_t count,
                                uint32_t *previous, uint32_t *error);
bool skse_win_sync_close(SkseWinSync *sync, uint32_t token);
/* Satisfy an immediate wait, consuming the selected auto-reset signal or count.
 * Returns 0x102 when no eligible signal is already present. */
uint32_t skse_win_sync_wait(SkseWinSync *sync, const uint32_t *tokens, size_t count, bool all, uint32_t *error);
#endif
