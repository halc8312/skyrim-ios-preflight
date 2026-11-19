/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winthread.h"

enum { OUT_OF_MEMORY = 8, INVALID_PARAMETER = 87 };

unsigned skse_win_thread_argument_count(SkseWinThreadOperation operation) {
    static const unsigned argument_counts[] = {
        0, 1, 1, 2, 0, 1, 0, // TLS and thread-local error state.
        1, 2, 1, 1, 1, 1     // Critical section operations.
    };
    _Static_assert(sizeof argument_counts / sizeof argument_counts[0] == SKSE_WIN_THREAD_OPERATION_COUNT,
                   "Win32 thread operation signatures");
    return (unsigned)operation < SKSE_WIN_THREAD_OPERATION_COUNT ? argument_counts[operation] : 0;
}

static uint32_t critical_section(SkseWinThread *thread, SkseWinThreadOperation op, uint32_t address) {
    SkseWinCriticalSection *found = 0, *available = 0;
    if (!address) return 0;
    for (unsigned i = 0; i < SKSE_WIN_CRITICAL_SECTIONS; ++i) {
        SkseWinCriticalSection *section = &thread->sections[i];
        if (section->address == address) {
            found = section;
            break;
        }
        if (!available && !section->address) available = section;
    }
    if (op == SKSE_WIN_CRITICAL_INIT || op == SKSE_WIN_CRITICAL_SPIN_INIT) {
        if (found) {
            thread->last_error = INVALID_PARAMETER;
            return 0;
        }
        if (!available) {
            thread->last_error = OUT_OF_MEMORY;
            return 0;
        }
        *available = (SkseWinCriticalSection){address, 0};
        return 1;
    }
    if (found) {
        switch (op) {
        case SKSE_WIN_CRITICAL_ENTER:
        case SKSE_WIN_CRITICAL_TRY:
            if (found->depth == UINT32_MAX) break;
            ++found->depth;
            return 1;
        case SKSE_WIN_CRITICAL_LEAVE:
            if (!found->depth) break;
            --found->depth;
            return 1;
        case SKSE_WIN_CRITICAL_DELETE:
            if (found->depth) break;
            *found = (SkseWinCriticalSection){0};
            return 1;
        default:
            break;
        }
    }
    thread->last_error = INVALID_PARAMETER;
    return 0;
}

static uint32_t allocate_slot(SkseWinThread *thread) {
    for (unsigned word = 0; word < SKSE_WIN_TLS_WORDS; ++word) {
        uint64_t available = ~thread->allocated[word];
        if (available) {
            unsigned bit = (unsigned)__builtin_ctzll(available);
            uint32_t index = word * 64 + bit;
            thread->allocated[word] |= UINT64_C(1) << bit;
            thread->value[index] = 0;
            return index;
        }
    }
    thread->last_error = OUT_OF_MEMORY;
    return UINT32_MAX;
}

uint32_t skse_win_thread_call(SkseWinThread *thread, SkseWinThreadOperation operation, uint32_t index, uint32_t value) {
    if (operation >= SKSE_WIN_CRITICAL_INIT)
        return critical_section(thread, operation, index);
    switch (operation) {
    case SKSE_WIN_TLS_ALLOC: return allocate_slot(thread);
    case SKSE_WIN_GET_LAST_ERROR: return thread->last_error;
    case SKSE_WIN_SET_LAST_ERROR: thread->last_error = index; return 0;
    case SKSE_WIN_CURRENT_THREAD_ID: return thread->id;
    default: break;
    }

    uint64_t mask = UINT64_C(1) << (index % 64);
    if (index >= SKSE_WIN_TLS_SLOTS || !(thread->allocated[index / 64] & mask)) {
        thread->last_error = INVALID_PARAMETER;
        return 0;
    }
    switch (operation) {
    case SKSE_WIN_TLS_GET:
        thread->last_error = 0;
        return thread->value[index];
    case SKSE_WIN_TLS_SET:
        thread->value[index] = value;
        return 1;
    case SKSE_WIN_TLS_FREE:
        thread->allocated[index / 64] &= ~mask;
        thread->value[index] = 0;
        return 1;
    default:
        thread->last_error = INVALID_PARAMETER;
        return 0;
    }
}
