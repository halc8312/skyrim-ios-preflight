/* SPDX-License-Identifier: GPL-3.0-only */
#include "lifecycle.h"
#include <stdatomic.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "enable the passive observer only on hosts with lock-free trace atomics");

/* One bounded trace per process. Reserve once, then make a complete packed row
 * visible. No waits, output, allocations, filenames, handles or addresses here. */
static _Atomic uint64_t reservations;
static _Atomic uint32_t published[SkyrimLifecycleCapacity];
static _Thread_local uint32_t current_ticket;

static void publish(uint64_t index, SkyrimLifecycleCall call, uint32_t returning, uint32_t outcome) {
    if (index >= SkyrimLifecycleCapacity) return;
    uint32_t row = call.site | (returning << 3) | (outcome << 4) | (call.ticket << 6) | (call.parent << 15);
    atomic_store_explicit(&published[index], row, memory_order_release);
}

SkyrimLifecycleCall skyrim_lifecycle_begin(uint32_t site) {
    uint64_t index = atomic_fetch_add_explicit(&reservations, 1, memory_order_relaxed);
    SkyrimLifecycleCall call = {site, index < SkyrimLifecycleCapacity ? (uint32_t)index + 1 : 0, current_ticket};
    current_ticket = call.ticket;
    publish(index, call, 0, SkyrimNoResult);
    return call;
}

void skyrim_lifecycle_end(SkyrimLifecycleCall call, uint32_t outcome) {
    uint64_t index = atomic_fetch_add_explicit(&reservations, 1, memory_order_relaxed);
    publish(index, call, 1, outcome);
    current_ticket = call.parent;
}

size_t skyrim_lifecycle_snapshot(SkyrimLifecycleRow *rows, size_t capacity, uint64_t *dropped) {
    uint64_t requested = atomic_load_explicit(&reservations, memory_order_relaxed);
    uint64_t excess = requested > SkyrimLifecycleCapacity ? requested - SkyrimLifecycleCapacity : 0;
    if (dropped) *dropped = excess;
    if (!rows) return 0;
    size_t count = (size_t)(requested - excess);
    if (count > capacity) count = capacity;
    for (size_t i = 0; i < count; ++i) {
        uint32_t row = atomic_load_explicit(&published[i], memory_order_acquire);
        if (!row) return i;
        rows[i] = (SkyrimLifecycleRow){row & 7u, (row >> 3) & 1u, (row >> 4) & 3u,
                                       (row >> 6) & 511u, (row >> 15) & 511u};
    }
    return count;
}
