/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winthread.h"

enum { OUT_OF_MEMORY = 8, INVALID_PARAMETER = 87 };

unsigned skse_win_thread_argument_count(SkseWinThreadOperation operation) {
    static const unsigned argument_counts[] = {
        0, 1, 1, 2, 0, 1, 0, // TLS and thread-local error state.
        1, 2, 1, 1, 1, 1, 3  // Critical section operations.
    };
    _Static_assert(sizeof argument_counts / sizeof argument_counts[0] == SKSE_WIN_THREAD_OPERATION_COUNT,
                   "Win32 thread operation signatures");
    return (unsigned)operation < SKSE_WIN_THREAD_OPERATION_COUNT ? argument_counts[operation] : 0;
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
