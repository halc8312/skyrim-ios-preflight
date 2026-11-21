/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(_WIN32)
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <sys/random.h>
#endif
#include "x86_winthread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WIN_INVALID_HANDLE = 6, OUT_OF_MEMORY = 8, INVALID_PARAMETER = 87 };

unsigned skse_win_thread_argument_count(SkseWinThreadOperation operation) {
    static const unsigned argument_counts[] = {
        0, 1, 1, 2, 0, 1, 0, 0, // TLS, thread-local error state and identity.
        1,                      // Historical file handle count.
        1, 2, 1, 1, 1, 1, 3,   // Critical section operations.
        1, 1, 1,                // UTC, counter and frequency.
        1, 1,                   // Process-local pointer coding.
        1, 1, 0, 0, 1,          // Startup information and capability queries.
        2, 5, 5                 // Console mode, read and write.
    };
    _Static_assert(sizeof argument_counts / sizeof argument_counts[0] == SKSE_WIN_THREAD_OPERATION_COUNT,
                   "Win32 thread operation signatures");
    return (unsigned)operation < SKSE_WIN_THREAD_OPERATION_COUNT ? argument_counts[operation] : 0;
}

int skse_win_clock_sample(SkseWinThreadOperation operation, uint64_t *sample) {
    if (operation == SKSE_WIN_SYSTEM_TIME) {
        // FILETIME counts 100 ns intervals from 1601-01-01 UTC.
        const int64_t epoch_delta = INT64_C(11644473600);
        const uint64_t ticks_per_second = UINT64_C(10000000);
        struct timespec now;
        if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0;
        const int64_t seconds = (int64_t)now.tv_sec;
        const int64_t limit = (int64_t)((UINT64_MAX - (ticks_per_second - 1)) / ticks_per_second);
        if (seconds < -epoch_delta || seconds > limit - epoch_delta) return 0;
        *sample = (uint64_t)(seconds + epoch_delta) * ticks_per_second + (uint64_t)now.tv_nsec / 100;
        return 1;
    }
#if defined(_WIN32)
    LARGE_INTEGER counter;
    if (operation == SKSE_WIN_PERFORMANCE_FREQUENCY) {
        if (!QueryPerformanceFrequency(&counter)) return 0;
    } else if (!QueryPerformanceCounter(&counter)) {
        return 0;
    }
    *sample = (uint64_t)counter.QuadPart;
#else
    const uint64_t ticks_per_second = UINT64_C(1000000000);
    if (operation == SKSE_WIN_PERFORMANCE_FREQUENCY) {
        *sample = ticks_per_second;
    } else {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            (uint64_t)now.tv_sec > (INT64_MAX - (ticks_per_second - 1)) / ticks_per_second) return 0;
        *sample = (uint64_t)now.tv_sec * ticks_per_second + (uint64_t)now.tv_nsec;
    }
#endif
    return 1;
}

static int read_entropy(uint32_t *word) {
#if defined(_WIN32)
    unsigned value;
    if (rand_s(&value)) return 0;
    *word = value;
    return 1;
#elif defined(__APPLE__)
    arc4random_buf(word, sizeof *word);
    return 1;
#else
    ssize_t size;
    do {
        size = getrandom(word, sizeof *word, 0);
    } while (size < 0 && errno == EINTR);
    return size == (ssize_t)sizeof *word;
#endif
}

int skse_win_transform_pointer(SkseWinThreadProcess *process, SkseWinThreadOperation operation,
                                uint32_t input, uint32_t *output) {
    if (!process->pointer_secret) {
        uint32_t secret;
        if (!read_entropy(&secret)) return 0;
        process->pointer_secret = secret ? secret : 1;
    }
    const uint32_t secret = process->pointer_secret;
    const unsigned shift = secret >> 27;
    const unsigned complement = (32 - shift) & 31;
    if (operation == SKSE_WIN_ENCODE_POINTER) {
        const uint32_t salted = input ^ secret;
        *output = (salted >> shift) | (salted << complement);
    } else {
        *output = ((input << shift) | (input >> complement)) ^ secret;
    }
    return 1;
}

void skse_win_startup_info(uint8_t output[SKSE_WIN_STARTUPINFO_BYTES]) {
    enum { STD_INPUT_OFFSET = 56 };
    /* PE32 fields are little-endian. No STARTF_USESTDHANDLES flag is set,
     * so the three standard handle fields retain their input bytes. */
    memset(output, 0, STD_INPUT_OFFSET);
    output[0] = SKSE_WIN_STARTUPINFO_BYTES;
}

uint32_t skse_win_std_handle(SkseWinThread *thread, uint32_t selector) {
    switch (selector) {
    case (uint32_t)-10: // STD_INPUT_HANDLE
    case (uint32_t)-11: // STD_OUTPUT_HANDLE
    case (uint32_t)-12: // STD_ERROR_HANDLE
        return 0;
    default:
        thread->last_error = WIN_INVALID_HANDLE;
        return UINT32_MAX;
    }
}

uint32_t skse_win_console_cp(SkseWinThread *thread) {
    thread->last_error = WIN_INVALID_HANDLE;
    return 0;
}

uint32_t skse_win_console_call(SkseWinThread *thread, SkseWinThreadOperation operation, bool disk_file) {
    thread->last_error = operation == SKSE_WIN_READ_CONSOLE_W && disk_file
        ? INVALID_PARAMETER : WIN_INVALID_HANDLE;
    return 0;
}

uint32_t skse_win_debugger_present(void) {
    return 0; // The isolated guest has no attached debugger.
}

uint32_t skse_win_processor_feature(uint32_t feature) {
    const uint32_t nx_enabled = 12; // PF_NX_ENABLED
    /* The interpreter rejects execution from guest data pages. Keep the
     * CPU capability queries conservative as it implements instruction
     * subsets and does not expose the host processor. */
    return feature == nx_enabled;
}

SkseWinCriticalResult skse_win_critical_section(SkseWinThread *thread, SkseWinThreadOperation op, uint32_t address) {
    SkseWinCriticalSection *found = 0, *available = 0;
    if (!address || !thread->id) {
        thread->last_error = INVALID_PARAMETER;
        return SKSE_WIN_CRITICAL_INVALID;
    }
    for (unsigned i = 0; i < SKSE_WIN_CRITICAL_SECTIONS; ++i) {
        SkseWinCriticalSection *section = &thread->process->sections[i];
        if (section->address == address) {
            found = section;
            break;
        }
        if (!available && !section->address) available = section;
    }
    if (op == SKSE_WIN_CRITICAL_INIT || op == SKSE_WIN_CRITICAL_SPIN_INIT) {
        if (found) {
            thread->last_error = INVALID_PARAMETER;
            return SKSE_WIN_CRITICAL_INVALID;
        }
        if (!available) {
            thread->last_error = OUT_OF_MEMORY;
            return SKSE_WIN_CRITICAL_INVALID;
        }
        *available = (SkseWinCriticalSection){.address = address};
        return SKSE_WIN_CRITICAL_ACQUIRED;
    }
    if (found) {
        switch (op) {
        case SKSE_WIN_CRITICAL_ENTER:
        case SKSE_WIN_CRITICAL_TRY:
            if (found->owner && found->owner != thread->id) return SKSE_WIN_CRITICAL_CONTENDED;
            if (found->depth == UINT32_MAX) break;
            found->owner = thread->id;
            ++found->depth;
            return SKSE_WIN_CRITICAL_ACQUIRED;
        case SKSE_WIN_CRITICAL_LEAVE:
            if (found->owner != thread->id || !found->depth) break;
            if (!--found->depth) found->owner = 0;
            return SKSE_WIN_CRITICAL_ACQUIRED;
        case SKSE_WIN_CRITICAL_DELETE:
            if (found->depth) break;
            *found = (SkseWinCriticalSection){0};
            return SKSE_WIN_CRITICAL_ACQUIRED;
        default:
            break;
        }
    }
    thread->last_error = INVALID_PARAMETER;
    return SKSE_WIN_CRITICAL_INVALID;
}

static uint32_t allocate_slot(SkseWinThread *thread) {
    SkseWinThreadProcess *process = thread->process;
    // Exhaustion is impossible within a diagnostic's work budget. Keep the
    // counter monotonic so an inactive thread cannot see values from a freed slot.
    if (process->generation == UINT32_MAX) {
        thread->last_error = OUT_OF_MEMORY;
        return UINT32_MAX;
    }
    for (unsigned word = 0; word < SKSE_WIN_TLS_WORDS; ++word) {
        uint64_t available = ~process->allocated[word];
        if (available) {
            unsigned bit = (unsigned)__builtin_ctzll(available);
            uint32_t index = word * 64 + bit;
            process->allocated[word] |= UINT64_C(1) << bit;
            process->slot_generation[index] = ++process->generation;
            return index;
        }
    }
    thread->last_error = OUT_OF_MEMORY;
    return UINT32_MAX;
}

uint32_t skse_win_thread_call(SkseWinThread *thread, SkseWinThreadOperation operation, uint32_t index, uint32_t value) {
    SkseWinThreadProcess *process = thread->process;
    switch (operation) {
    case SKSE_WIN_TLS_ALLOC: return allocate_slot(thread);
    case SKSE_WIN_GET_LAST_ERROR: return thread->last_error;
    case SKSE_WIN_SET_LAST_ERROR: thread->last_error = index; return 0;
    case SKSE_WIN_CURRENT_THREAD_ID: return thread->id;
    case SKSE_WIN_CURRENT_PROCESS_ID: return 4096; // Each isolated VM has one process.
    case SKSE_WIN_SET_HANDLE_COUNT:
        // Win32 returns the request and leaves the handle table and last error intact.
        return index;
    default: break;
    }

    uint64_t mask = UINT64_C(1) << (index % 64);
    if (index >= SKSE_WIN_TLS_SLOTS || !(process->allocated[index / 64] & mask)) {
        thread->last_error = INVALID_PARAMETER;
        return 0;
    }
    switch (operation) {
    case SKSE_WIN_TLS_GET:
        thread->last_error = 0;
        return thread->value_generation[index] == process->slot_generation[index] ? thread->value[index] : 0;
    case SKSE_WIN_TLS_SET:
        thread->value[index] = value;
        thread->value_generation[index] = process->slot_generation[index];
        return 1;
    case SKSE_WIN_TLS_FREE:
        process->allocated[index / 64] &= ~mask;
        return 1;
    default:
        thread->last_error = INVALID_PARAMETER;
        return 0;
    }
}
