/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdatomic.h>
#include "performance_metrics.h"

/* Host GFile only, never part of the guest/CPU ABI. Created and destroyed with
 * the existing file object; this does not repair upstream lookup/close races.
 * Concurrent valid writes do not race on this observation state.
 * Filename classification is a candidate, NOT proof of an autosave. */
typedef struct SkyrimSaveWriteProbe {
    unsigned candidate;
    _Atomic uint64_t first_observed_ns;
    _Atomic uint64_t last_write_start_ns;
} SkyrimSaveWriteProbe;

static inline unsigned skyrim_save_ascii_lower(unsigned ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static inline int skyrim_save_suffix(const char *s, size_t len, const char *suffix, size_t n) {
    if (len < n) return 0;
    for (size_t i = 0; i < n; ++i)
        if (skyrim_save_ascii_lower((unsigned char)s[len - n + i]) != (unsigned char)suffix[i]) return 0;
    return 1;
}

static inline void skyrim_save_write_init(SkyrimSaveWriteProbe *p, const char *path, size_t capacity) {
    size_t len = 0;
    if (path) while (len < capacity && path[len]) ++len;
    p->candidate = path && len < capacity &&
        (skyrim_save_suffix(path, len, ".ess", 4) ||
         skyrim_save_suffix(path, len, ".ess.tmp", 8) ||
         skyrim_save_suffix(path, len, ".ess.bak", 8));
    atomic_init(&p->first_observed_ns, 0);
    atomic_init(&p->last_write_start_ns, 0);
}

static inline uint64_t skyrim_save_write_begin(SkyrimSaveWriteProbe *p) {
    uint64_t now = skyrim_perf_now_ns();
    if (p->candidate && now) {
        uint64_t expected = 0;
        /* One attempt, no waiting. Under concurrent writes this is the first
         * successful observer, not necessarily the earliest syscall. */
        atomic_compare_exchange_strong_explicit(&p->first_observed_ns, &expected, now,
                                               memory_order_relaxed, memory_order_relaxed);
        /* Overlapping callers can reach the probe in a different order from
         * their clock reads. Keep the latest start without touching p after I/O. */
        uint64_t last = atomic_load_explicit(&p->last_write_start_ns, memory_order_relaxed);
        while (last < now && !atomic_compare_exchange_strong_explicit(
                &p->last_write_start_ns, &last, now, memory_order_relaxed, memory_order_relaxed)) {}
    }
    return now;
}

static inline void skyrim_save_write_end(unsigned candidate, uint64_t start, int failed, int ex) {
    /* The caller captures the class before I/O. Do not extend the original
     * GFile lifetime by touching it after a potentially blocking syscall. */
    unsigned kind = candidate ? (ex ? SKYRIM_PERF_SAVE_CANDIDATE_WRITE_EX : SKYRIM_PERF_SAVE_CANDIDATE_WRITE)
                                : (ex ? SKYRIM_PERF_WRITE_FILE_EX : SKYRIM_PERF_WRITE_FILE);
    skyrim_perf_end(kind, start, failed);
}

static inline uint64_t skyrim_save_write_close_begin(const SkyrimSaveWriteProbe *p) {
    if (!p->candidate || !atomic_load_explicit(&p->first_observed_ns, memory_order_relaxed)) return 0;
    return skyrim_perf_now_ns();
}

static inline void skyrim_save_write_close(SkyrimSaveWriteProbe *p, uint64_t close_begin) {
    uint64_t end = close_begin ? skyrim_perf_now_ns() : 0;
    const uint64_t first = atomic_exchange_explicit(&p->first_observed_ns, 0, memory_order_relaxed);
    if (!first) return;
    if (!close_begin) end = skyrim_perf_now_ns();
    skyrim_perf_interval(SKYRIM_PERF_SAVE_CANDIDATE_WINDOW, first, end, 0, 0);
    const uint64_t last = atomic_load_explicit(&p->last_write_start_ns, memory_order_relaxed);
    if (last < first || last > close_begin || close_begin > end) return;

    /* The middle interval starts immediately before the latest write and ends
     * at close entry. That write can still be in flight when the handle is freed.
     * All three parts use the same clock boundaries. A shared fd, a failed
     * clock read, or unordered observations leave only the existing window. */
    skyrim_perf_interval(SKYRIM_PERF_SAVE_FIRST_TO_LAST_WRITE_START, first, last, 0, 0);
    skyrim_perf_interval(SKYRIM_PERF_SAVE_LAST_WRITE_START_TO_CLOSE, last, close_begin, 0, 0);
    skyrim_perf_interval(SKYRIM_PERF_SAVE_CLOSE_CALL, close_begin, end, 0, 0);
}
