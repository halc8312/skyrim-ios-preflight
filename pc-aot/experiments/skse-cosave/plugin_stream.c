/* SPDX-License-Identifier: GPL-3.0-only */
#include "plugin_stream.h"
#include "wire_read.h"
#include <stdlib.h>
#include <string.h>

enum { BLOCK = 12, FILE_PREFIX = 20 };
void skse_stream_put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static uint8_t *reserve(SkseRecordWriter *w, size_t size) {
    SkseEncodedRecords *all = w->all;
    const size_t limit = SKSE_PAIR_MAX_COMPANION - FILE_PREFIX;
    if (size > limit - all->used) { w->error = SKSE_PAIR_LIMIT; return NULL; }
    size_t needed = all->used + size;
    if (needed > all->capacity) {
        size_t capacity = all->capacity ? all->capacity * 2 : 256;
        if (capacity < needed) capacity = needed;
        if (capacity > limit) capacity = limit;
        uint8_t *larger = realloc(all->data, capacity);
        if (!larger) { w->error = SKSE_PAIR_STORAGE; return NULL; }
        all->data = larger;
        all->capacity = capacity;
    }
    uint8_t *p = all->data + all->used;
    all->used = needed;
    w->section->length += size;
    return p;
}

int skse_record_open(SkseRecordWriter *w, uint32_t type, uint32_t version) {
    if (!w || w->error != SKSE_PAIR_OK) return 0;
    int first = w->section->count == 0;
    unsigned headers = first ? 2 : 1;
    if (w->all->chunks > SKSE_PAIR_MAX_HEADERS - headers) { w->error = SKSE_PAIR_LIMIT; return 0; }
    uint8_t *p = reserve(w, (size_t)headers * BLOCK);
    if (!p) return 0;
    if (first) {
        w->section->offset = (size_t)(p - w->all->data);
        skse_stream_put32(p, w->uid);
        p += BLOCK;
    }
    w->header = (size_t)(p - w->all->data);
    skse_stream_put32(p, type);
    skse_stream_put32(p + 4, version);
    skse_stream_put32(p + 8, 0);
    w->section->count++;
    w->all->chunks += headers;
    return 1;
}
int skse_record_append(SkseRecordWriter *w, const void *data, uint32_t length) {
    if (!w || w->error != SKSE_PAIR_OK) return 0;
    if (!w->section->count || (length && !data)) { w->error = SKSE_PAIR_ARGUMENT; return 0; }
    if (!length) return 1;
    uint8_t *p = reserve(w, length);
    if (!p) return 0;
    memcpy(p, data, length);
    skse_stream_put32(w->all->data + w->header + 8, (uint32_t)(w->all->used - w->header - BLOCK));
    return 1;
}
int skse_record_write(SkseRecordWriter *w, uint32_t type, uint32_t version, const void *data, uint32_t length) {
    return skse_record_open(w, type, version) && skse_record_append(w, data, length);
}

int skse_record_next(SkseRecordReader *r, uint32_t *type, uint32_t *version, uint32_t *length) {
    if (!r || r->error != SKSE_PAIR_OK) return 0;
    if (!type || !version || !length) { r->error = SKSE_PAIR_ARGUMENT; return 0; }
    r->payload = NULL;
    r->left = 0;
    if (!r->owner.count) return 0;
    const uint8_t *p = r->owner.data;
    *type = skse_wire_le32(p);
    *version = skse_wire_le32(p + 4);
    *length = skse_wire_le32(p + 8);
    r->payload = p + BLOCK;
    r->left = *length;
    r->owner.data = r->payload + *length;
    r->owner.count--;
    return 1;
}
uint32_t skse_record_read(SkseRecordReader *r, void *data, uint32_t capacity) {
    if (!r || r->error != SKSE_PAIR_OK) return 0;
    if (!r->payload || (!data && capacity)) { r->error = SKSE_PAIR_ARGUMENT; return 0; }
    uint32_t copied = capacity < r->left ? capacity : r->left;
    if (copied) {
        memcpy(data, r->payload, copied);
        r->payload += copied;
        r->left -= copied;
    }
    return copied;
}
SkseModResolution skse_record_resolve_form(SkseRecordReader *r, uint32_t saved, uint32_t *current) {
    return r ? skse_mod_resolve_form(r->map, saved, current) : SKSE_MOD_ARGUMENT;
}
SkseModResolution skse_record_resolve_handle(SkseRecordReader *r, uint64_t saved, uint64_t *current) {
    return r ? skse_mod_resolve_handle(r->map, saved, current) : SKSE_MOD_ARGUMENT;
}
