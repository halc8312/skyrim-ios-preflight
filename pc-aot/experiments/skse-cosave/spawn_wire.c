/* SPDX-License-Identifier: GPL-3.0-only */
#include "spawn_wire.h"

enum { WIN32_SPAWN_DATA = 40 };
SkseObjectResult skse_spawn_body(SkseWire *cursor, SkseSpawnBody *body) {
    const uint8_t *header = skse_wire_take(cursor, 8);
    if (!header) return SKSE_OBJECT_INVALID;
    body->stack = skse_wire_le32(header);
    body->count = skse_wire_le32(header + 4);
    if (body->count > SKSE_OBJECT_MAX_ARGUMENTS) return SKSE_OBJECT_LIMIT;
    body->rows = skse_wire_take(cursor, (size_t)body->count * WIN32_SPAWN_DATA);
    if (!body->rows) return SKSE_OBJECT_INVALID;
    const uint8_t *p = body->rows;
    for (uint32_t i = 0; i < body->count; ++i, p += WIN32_SPAWN_DATA) {
        if (p[36] > 1 || p[37] > 1) return SKSE_OBJECT_INVALID;
    }
    return SKSE_OBJECT_OK;
}
uint32_t skse_spawn_observe(const SkseSpawnBody *body, SkseStoredSpawnTask *task, const SkseModMap *map,
                           SkseSpawnTaskObserver on_task, SkseSpawnEntryObserver on_entry, void *context) {
    task->latent_stack = body->stack;
    task->entries = body->count;
    if (on_task) on_task(context, task);
    uint32_t unresolved = 0;
    const uint8_t *p = body->rows;
    for (uint32_t i = 0; i < body->count; ++i, p += WIN32_SPAWN_DATA) {
        SkseStoredSpawnEntry row = {.saved_form = skse_wire_le32(p)};
        row.resolution = skse_mod_resolve_form(map, row.saved_form, &row.mapped_form);
        if (row.resolution != SKSE_MOD_RESOLVED && row.resolution != SKSE_MOD_DYNAMIC) ++unresolved;
        if (on_entry) {
            row.target_ref_handle = skse_wire_le32(p + 4);
            for (size_t j = 0; j < 3; ++j) {
                row.position_bits[j] = skse_wire_le32(p + 8 + 4 * j);
                row.rotation_bits[j] = skse_wire_le32(p + 20 + 4 * j);
            }
            row.count = skse_wire_le32(p + 32);
            row.force_persist = p[36];
            row.initially_disabled = p[37];
            on_entry(context, task, i, &row);
        }
    }
    return unresolved;
}
