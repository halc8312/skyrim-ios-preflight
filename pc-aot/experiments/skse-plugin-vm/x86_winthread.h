/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINTHREAD_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINTHREAD_H

#include <stdint.h>

enum {
    SKSE_WIN_TLS_SLOTS = 1088, SKSE_WIN_TLS_WORDS = SKSE_WIN_TLS_SLOTS / 64,
    SKSE_WIN_CRITICAL_SECTIONS = 64, SKSE_WIN_CRITICAL_SECTION_BYTES = 24
};

typedef enum {
    SKSE_WIN_TLS_ALLOC, SKSE_WIN_TLS_FREE, SKSE_WIN_TLS_GET, SKSE_WIN_TLS_SET,
    SKSE_WIN_GET_LAST_ERROR, SKSE_WIN_SET_LAST_ERROR, SKSE_WIN_CURRENT_THREAD_ID,
    SKSE_WIN_CRITICAL_INIT, SKSE_WIN_CRITICAL_SPIN_INIT, SKSE_WIN_CRITICAL_ENTER,
    SKSE_WIN_CRITICAL_LEAVE, SKSE_WIN_CRITICAL_TRY, SKSE_WIN_CRITICAL_DELETE,
    SKSE_WIN_CRITICAL_INIT_EX, SKSE_WIN_THREAD_OPERATION_COUNT
} SkseWinThreadOperation;

typedef struct {
    uint32_t address, owner, depth;
} SkseWinCriticalSection;

typedef struct {
    uint64_t allocated[SKSE_WIN_TLS_WORDS];
    uint32_t generation, slot_generation[SKSE_WIN_TLS_SLOTS];
    SkseWinCriticalSection sections[SKSE_WIN_CRITICAL_SECTIONS];
} SkseWinThreadProcess;

/* The selected images share a process model. Each logical thread has its
 * own TLS values and last-error state, stored as 32-bit guest data. */
typedef struct {
    SkseWinThreadProcess *process;
    uint32_t value[SKSE_WIN_TLS_SLOTS];
    uint32_t value_generation[SKSE_WIN_TLS_SLOTS];
    uint32_t id, last_error;
} SkseWinThread;

typedef enum {
    SKSE_WIN_CRITICAL_ACQUIRED, SKSE_WIN_CRITICAL_CONTENDED, SKSE_WIN_CRITICAL_INVALID
} SkseWinCriticalResult;

unsigned skse_win_thread_argument_count(SkseWinThreadOperation operation);
uint32_t skse_win_thread_call(SkseWinThread *thread, SkseWinThreadOperation operation, uint32_t index, uint32_t value);
SkseWinCriticalResult skse_win_critical_section(SkseWinThread *thread, SkseWinThreadOperation operation, uint32_t address);
#endif
