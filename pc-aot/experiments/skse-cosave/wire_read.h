/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "core_snapshot.h"
#include <string.h>

typedef struct { const uint8_t *p; size_t left; } SkseWire;
static inline uint32_t skse_wire_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline const uint8_t *skse_wire_take(SkseWire *cursor, size_t n) {
    if (n > cursor->left) return NULL;
    const uint8_t *data = cursor->p;
    cursor->p += n;
    cursor->left -= n;
    return data;
}
static inline int skse_wire_name16(SkseWire *cursor, SkseCoreName *name) {
    const uint8_t *len = skse_wire_take(cursor, 2);
    if (!len) return 0;
    size_t size = (size_t)len[0] | (size_t)len[1] << 8;
    if (!size || size > 256) return 0;
    const uint8_t *bytes = skse_wire_take(cursor, size);
    if (!bytes || memchr(bytes, 0, size)) return 0;
    *name = (SkseCoreName){bytes, size};
    return 1;
}
