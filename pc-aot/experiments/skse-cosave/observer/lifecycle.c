/* SPDX-License-Identifier: GPL-3.0-only */
#include "lifecycle.h"
#include <stdatomic.h>
#include <string.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "enable the passive observer only on hosts with lock-free trace atomics");

/* One bounded trace per process. Reserve once, then make a complete packed row
 * visible. No waits, output, allocations, filenames, handles or addresses here. */
static _Atomic uint64_t reservations;
static _Atomic uint32_t published[SkyrimLifecycleCapacity];
static _Thread_local uint32_t current_ticket;
static _Atomic uint64_t last_generation;
static _Thread_local SkyrimLifecycleScope *active_scope;

bool skyrim_lifecycle_scope_open(SkyrimLifecycleScope *scope) {
    if (!scope || active_scope || current_ticket) return false;
    uint64_t previous = atomic_load_explicit(&last_generation, memory_order_relaxed);
    do {
        if (previous == UINT64_MAX) return false;
    } while (!atomic_compare_exchange_weak_explicit(&last_generation, &previous, previous + 1,
                                                   memory_order_relaxed, memory_order_relaxed));
    scope->count = 0;
    scope->current = 0;
    scope->invalid = false;
    scope->generation = previous + 1;
    active_scope = scope;
    return true;
}

bool skyrim_lifecycle_scope_close(SkyrimLifecycleScope *scope) {
    if (!scope || active_scope != scope) return false;
    active_scope = NULL;
    return !scope->invalid && !scope->current;
}

SkyrimLifecycleCall skyrim_lifecycle_scope_current(void) {
    const SkyrimLifecycleScope *scope = active_scope;
    if (!scope || scope->invalid) return (SkyrimLifecycleCall){0};
    if (!scope->current) return (SkyrimLifecycleCall){.generation = scope->generation};
    const SkyrimLifecycleRow *row = &scope->rows[scope->current - 1];
    return (SkyrimLifecycleCall){row->site, row->ticket, row->parent, scope->generation};
}

static SkyrimLifecycleCall scoped_begin(SkyrimLifecycleScope *scope, uint32_t site) {
    SkyrimLifecycleCall call = {.site = site, .generation = scope->generation};
    if (scope->invalid || scope->count == SkyrimLifecycleCapacity ||
        site < SkyrimSaveSlot || site > SkyrimRegisterPapyrus) {
        scope->invalid = true;
        return call;
    }
    call.parent = scope->current;
    call.ticket = (uint32_t)scope->count + 1;
    scope->rows[scope->count++] = (SkyrimLifecycleRow){site, 0, SkyrimNoResult, call.ticket, call.parent};
    scope->current = call.ticket;
    return call;
}

static void scoped_end(SkyrimLifecycleCall call, uint32_t outcome) {
    SkyrimLifecycleScope *scope = active_scope;
    if (!scope || scope->generation != call.generation || scope->invalid) return;
    if (!call.ticket || call.ticket != scope->current || scope->count == SkyrimLifecycleCapacity ||
        outcome > SkyrimReturnedTrue) {
        scope->invalid = true;
        return;
    }
    const SkyrimLifecycleRow *entry = &scope->rows[call.ticket - 1];
    if (entry->site != call.site || entry->parent != call.parent) {
        scope->invalid = true;
        return;
    }
    scope->rows[scope->count++] = (SkyrimLifecycleRow){call.site, 1, outcome, call.ticket, call.parent};
    scope->current = call.parent;
}

static void publish(uint64_t index, SkyrimLifecycleCall call, uint32_t returning, uint32_t outcome) {
    if (index >= SkyrimLifecycleCapacity) return;
    uint32_t row = call.site | (returning << 3) | (outcome << 4) | (call.ticket << 6) | (call.parent << 15);
    atomic_store_explicit(&published[index], row, memory_order_release);
}

SkyrimLifecycleCall skyrim_lifecycle_begin(uint32_t site) {
    if (active_scope) return scoped_begin(active_scope, site);
    uint64_t index = atomic_fetch_add_explicit(&reservations, 1, memory_order_relaxed);
    SkyrimLifecycleCall call = {site, index < SkyrimLifecycleCapacity ? (uint32_t)index + 1 : 0, current_ticket, 0};
    current_ticket = call.ticket;
    publish(index, call, 0, SkyrimNoResult);
    return call;
}

void skyrim_lifecycle_end(SkyrimLifecycleCall call, uint32_t outcome) {
    if (call.generation) {
        scoped_end(call, outcome);
        return;
    }
    uint64_t index = atomic_fetch_add_explicit(&reservations, 1, memory_order_relaxed);
    publish(index, call, 1, outcome);
    current_ticket = call.parent;
}

size_t skyrim_lifecycle_snapshot(SkyrimLifecycleRow *rows, size_t capacity, uint64_t *dropped) {
    if (active_scope) {
        const SkyrimLifecycleScope *scope = active_scope;
        if (dropped) *dropped = scope->invalid || (rows && scope->count > capacity);
        if (!rows) return 0;
        size_t count = scope->count <= capacity ? scope->count : capacity;
        memcpy(rows, scope->rows, count * sizeof(*rows));
        return count;
    }
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
