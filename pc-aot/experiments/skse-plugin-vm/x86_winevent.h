/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINEVENT_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINEVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SKSE_WIN_EVENT_LIMIT = 16, SKSE_WIN_WAIT_TIMEOUT = 0x102 };

typedef enum {
    SKSE_WIN_EVENT_CREATE, SKSE_WIN_EVENT_SET, SKSE_WIN_EVENT_RESET,
    SKSE_WIN_WAIT_ONE, SKSE_WIN_WAIT_ONE_EX, SKSE_WIN_WAIT_MANY, SKSE_WIN_WAIT_MANY_EX,
    SKSE_WIN_EVENT_OPERATION_COUNT
} SkseWinEventOperation;

typedef struct {
    uint32_t token;
    bool manual, signaled;
} SkseWinEvent;

typedef struct {
    uint32_t next;
    SkseWinEvent objects[SKSE_WIN_EVENT_LIMIT];
} SkseWinEvents;

uint32_t skse_win_event_create(SkseWinEvents *events, bool manual, bool signaled);
bool skse_win_event_close(SkseWinEvents *events, uint32_t token);
bool skse_win_event_signal(SkseWinEvents *events, uint32_t token, bool signaled);
/* Satisfy an immediate wait, consuming only the selected auto-reset signals.
 * Returns 0x102 when no eligible signal is already present. */
uint32_t skse_win_event_wait(SkseWinEvents *events, const uint32_t *tokens, size_t count, bool all, uint32_t *error);
#endif
