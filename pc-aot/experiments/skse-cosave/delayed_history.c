/* SPDX-License-Identifier: GPL-3.0-only */
#include "spawn_wire.h"
#include "core_view.h"
#include <string.h>

enum { MODS = 0x4d4f4453, DFMG = 0x44464d47, FUNQ = 0x46554e51, WLST = 0x574c5354,
       OBJE = 0x4f424a45, END = 0x5f5f5f5f };
typedef enum { BeforeManager, NeedQueue, InQueue, NeedWaitList, InWaitList, NeedEnd, Complete } DelayPhase;
typedef struct {
    const SkseModMap *map;
    SkseDelayedSummary summary;
    SkseSpawnTaskObserver task;
    SkseSpawnEntryObserver entry;
    void *context;
    DelayPhase phase;
    SkseSpawnLocation group;
    uint32_t left, next_index, total;
    int saw_mods;
    SkseObjectResult error;
} DelayWalk;

static SkseObjectResult read_count(DelayWalk *w, SkseWire *cursor, uint32_t *out) {
    const uint8_t *bytes = skse_wire_take(cursor, 4);
    if (!bytes) return SKSE_OBJECT_INVALID;
    *out = skse_wire_le32(bytes);
    if (*out > SKSE_OBJECT_MAX_SLOTS - w->total) return SKSE_OBJECT_LIMIT;
    w->total += *out;
    w->left = *out;
    w->next_index = 0;
    return SKSE_OBJECT_OK;
}

/* With no entries in a list, the following count stays in this record.
 * Otherwise the following count is appended to the last OBJE in the list. */
static SkseObjectResult enter_wait_groups(DelayWalk *w, SkseWire *cursor, unsigned first) {
    uint32_t *counts[] = {&w->summary.incoming, &w->summary.waiting, &w->summary.ready};
    for (unsigned g = first; g <= SKSE_SPAWN_READY; ++g) {
        SkseObjectResult result = read_count(w, cursor, counts[g - SKSE_SPAWN_INCOMING]);
        if (result != SKSE_OBJECT_OK) return result;
        if (w->left) {
            w->group = (SkseSpawnLocation)g;
            w->phase = InWaitList;
            return cursor->left ? SKSE_OBJECT_INVALID : SKSE_OBJECT_OK;
        }
    }
    w->phase = NeedEnd;
    return cursor->left ? SKSE_OBJECT_INVALID : SKSE_OBJECT_OK;
}
static SkseObjectResult inspect_task(DelayWalk *w, SkseWire *cursor) {
    const uint8_t *prefix = skse_wire_take(cursor, 4);
    if (!prefix) return SKSE_OBJECT_INVALID;
    uint32_t size = skse_wire_le32(prefix);
    if (!size || size > 1024) return SKSE_OBJECT_INVALID;
    const uint8_t *name = skse_wire_take(cursor, size);
    if (!name || memchr(name, 0, size)) return SKSE_OBJECT_INVALID;
    if (size != sizeof("SpawnerTaskFunctor") - 1 || memcmp(name, "SpawnerTaskFunctor", size))
        return SKSE_OBJECT_UNKNOWN_CLASS;
    SkseSpawnBody body;
    SkseObjectResult result = skse_spawn_body(cursor, &body);
    if (result != SKSE_OBJECT_OK) return result;
    if (body.count > SKSE_OBJECT_MAX_ARGUMENTS - w->summary.entries) return SKSE_OBJECT_LIMIT;
    SkseStoredSpawnTask task = {.location = w->group, .index = w->next_index++};
    if (w->group == SKSE_SPAWN_INCOMING || w->group == SKSE_SPAWN_WAITING) {
        const uint8_t *delay = skse_wire_take(cursor, 8);
        if (!delay) return SKSE_OBJECT_INVALID;
        task.has_delay = 1;
        task.delay_bits = skse_wire_le64(delay);
    }
    w->summary.entries += body.count;
    w->summary.unresolved_forms += skse_spawn_observe(&body, &task, w->map, w->task, w->entry, w->context);
    if (--w->left == 0) {
        if (w->phase == InWaitList) return enter_wait_groups(w, cursor, (unsigned)w->group + 1);
        w->phase = NeedWaitList;
    }
    return cursor->left ? SKSE_OBJECT_INVALID : SKSE_OBJECT_OK;
}
static SkseObjectResult read_row(DelayWalk *w, const SksePairRecord *row) {
    if (row->type == DFMG) {
        if (w->phase != BeforeManager) return SKSE_OBJECT_DUPLICATE;
        if (!w->saw_mods) return SKSE_OBJECT_OUT_OF_ORDER;
        if (row->version != 1) return SKSE_OBJECT_UNSUPPORTED_VERSION;
        if (row->length) return SKSE_OBJECT_INVALID;
        w->phase = NeedQueue;
        return SKSE_OBJECT_OK;
    }
    if (w->phase == BeforeManager) {
        if (row->type == MODS) w->saw_mods = 1;
        return SKSE_OBJECT_OK;
    }
    if (w->phase == Complete) return SKSE_OBJECT_OK;
    if (row->version != 1) return SKSE_OBJECT_UNSUPPORTED_VERSION;
    SkseWire cursor = {row->data, row->length};
    switch (w->phase) {
        case NeedQueue: {
            if (row->type != FUNQ) return SKSE_OBJECT_INVALID;
            SkseObjectResult result = read_count(w, &cursor, &w->summary.queued);
            if (result != SKSE_OBJECT_OK) return result;
            w->phase = w->left ? InQueue : NeedWaitList;
            w->group = SKSE_SPAWN_QUEUE;
            return cursor.left ? SKSE_OBJECT_INVALID : SKSE_OBJECT_OK;
        }
        case NeedWaitList:
            return row->type == WLST ? enter_wait_groups(w, &cursor, SKSE_SPAWN_INCOMING) : SKSE_OBJECT_INVALID;
        case InQueue: case InWaitList:
            return row->type == OBJE ? inspect_task(w, &cursor) : SKSE_OBJECT_INVALID;
        case NeedEnd:
            if (row->type != END || row->length) return SKSE_OBJECT_INVALID;
            w->phase = Complete;
            return SKSE_OBJECT_OK;
        default: return SKSE_OBJECT_INVALID;
    }
}
static int visit_row(void *context, const SksePairRecord *row) {
    DelayWalk *w = context;
    w->error = read_row(w, row);
    return w->error == SKSE_OBJECT_OK;
}
static SkseObjectResult scan(const SkseCoreSnapshot *snapshot, SkseDelayedSummary *out,
                            SkseSpawnTaskObserver task, SkseSpawnEntryObserver entry, void *context) {
    DelayWalk walk = {.map = &snapshot->map, .task = task, .entry = entry, .context = context};
    skse_records_each(&snapshot->core, visit_row, &walk);
    if (walk.error != SKSE_OBJECT_OK) return walk.error;
    if (walk.phase == BeforeManager) return SKSE_OBJECT_MISSING;
    if (walk.phase != Complete) return SKSE_OBJECT_INVALID;
    *out = walk.summary;
    return SKSE_OBJECT_OK;
}
SkseObjectResult skse_core_delayed_tasks(const SkseCoreSnapshot *snapshot, SkseDelayedSummary *out,
                                       SkseSpawnTaskObserver task, SkseSpawnEntryObserver entry, void *context) {
    if (!out) return SKSE_OBJECT_ARGUMENT;
    *out = (SkseDelayedSummary){0};
    if (!snapshot) return SKSE_OBJECT_ARGUMENT;
    SkseObjectResult result = scan(snapshot, out, NULL, NULL, NULL);
    if (result == SKSE_OBJECT_OK && (task || entry)) result = scan(snapshot, out, task, entry, context);
    return result;
}
