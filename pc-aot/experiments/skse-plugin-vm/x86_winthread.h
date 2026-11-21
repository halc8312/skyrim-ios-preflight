/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINTHREAD_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINTHREAD_H

#include <stdbool.h>
#include <stdint.h>

enum {
    SKSE_WIN_TLS_SLOTS = 1088, SKSE_WIN_TLS_WORDS = SKSE_WIN_TLS_SLOTS / 64,
    SKSE_WIN_CRITICAL_SECTIONS = 64, SKSE_WIN_CRITICAL_SECTION_BYTES = 24,
    SKSE_WIN_STARTUPINFO_BYTES = 68
};

typedef enum {
    SKSE_WIN_TLS_ALLOC, SKSE_WIN_TLS_FREE, SKSE_WIN_TLS_GET, SKSE_WIN_TLS_SET,
    SKSE_WIN_GET_LAST_ERROR, SKSE_WIN_SET_LAST_ERROR, SKSE_WIN_CURRENT_THREAD_ID, SKSE_WIN_CURRENT_PROCESS_ID,
    SKSE_WIN_SET_HANDLE_COUNT,
    SKSE_WIN_CRITICAL_INIT, SKSE_WIN_CRITICAL_SPIN_INIT, SKSE_WIN_CRITICAL_ENTER,
    SKSE_WIN_CRITICAL_LEAVE, SKSE_WIN_CRITICAL_TRY, SKSE_WIN_CRITICAL_DELETE,
    SKSE_WIN_CRITICAL_INIT_EX, SKSE_WIN_SYSTEM_TIME, SKSE_WIN_PERFORMANCE_COUNTER, SKSE_WIN_PERFORMANCE_FREQUENCY,
    SKSE_WIN_ENCODE_POINTER, SKSE_WIN_DECODE_POINTER,
    SKSE_WIN_GET_STARTUP_INFO, SKSE_WIN_GET_STD_HANDLE, SKSE_WIN_GET_CONSOLE_CP,
    SKSE_WIN_IS_DEBUGGER_PRESENT, SKSE_WIN_PROCESSOR_FEATURE,
    SKSE_WIN_GET_CONSOLE_MODE, SKSE_WIN_READ_CONSOLE_W, SKSE_WIN_WRITE_CONSOLE_W,
    SKSE_WIN_THREAD_OPERATION_COUNT
} SkseWinThreadOperation;

typedef struct {
    uint32_t address, owner, depth;
} SkseWinCriticalSection;

typedef struct {
    uint64_t allocated[SKSE_WIN_TLS_WORDS];
    uint32_t generation, slot_generation[SKSE_WIN_TLS_SLOTS];
    SkseWinCriticalSection sections[SKSE_WIN_CRITICAL_SECTIONS];
    uint32_t pointer_secret;
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
int skse_win_clock_sample(SkseWinThreadOperation operation, uint64_t *sample);
int skse_win_transform_pointer(SkseWinThreadProcess *process, SkseWinThreadOperation operation,
                                uint32_t input, uint32_t *output);

/* The guest starts as a GUI process with no inherited standard handles,
 * console, debugger or shell-supplied startup strings. The caller
 * validates the full PE32 STARTUPINFOA or STARTUPINFOW output range.
 * No startup strings means both APIs have the same guest representation. */
void skse_win_startup_info(uint8_t output[SKSE_WIN_STARTUPINFO_BYTES]);
uint32_t skse_win_std_handle(SkseWinThread *thread, uint32_t selector);
uint32_t skse_win_console_cp(SkseWinThread *thread);
/* The GUI guest has no console. Handle the three console queries/transfers
 * that fail in that state, with a distinct ReadConsoleW error for an existing
 * disk file. The caller handles the unsupported optional input controls.
 * Neither character buffers nor output DWORDs are touched. */
uint32_t skse_win_console_call(SkseWinThread *thread, SkseWinThreadOperation operation, bool disk_file);
uint32_t skse_win_debugger_present(void);
uint32_t skse_win_processor_feature(uint32_t feature);
#endif
