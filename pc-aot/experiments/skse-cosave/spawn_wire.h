/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "object_catalog.h"
#include "wire_read.h"

typedef struct { uint32_t stack, count; const uint8_t *rows; } SkseSpawnBody;
static inline uint64_t skse_wire_le64(const uint8_t *p) {
    return skse_wire_le32(p) | ((uint64_t)skse_wire_le32(p + 4) << 32);
}
SkseObjectResult skse_spawn_body(SkseWire *cursor, SkseSpawnBody *body);
uint32_t skse_spawn_observe(const SkseSpawnBody *body, SkseStoredSpawnTask *task, const SkseModMap *map,
                           SkseSpawnTaskObserver on_task, SkseSpawnEntryObserver on_entry, void *context);
