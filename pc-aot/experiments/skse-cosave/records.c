/* SPDX-License-Identifier: GPL-3.0-only
 * In-memory adaptation of classic_cosave.c in the reviewed Classic v1 port. */
#include "records.h"
#include <stdlib.h>
#include <string.h>

enum { HEADER = 20, BLOCK = 12 };
typedef struct {
    size_t first, end;
    uint32_t plugins, records, owned_records;
} Span;

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static uint8_t *block(uint8_t *p, uint32_t id, uint32_t version, uint32_t length) {
    put32(p, id); put32(p + 4, version); put32(p + 8, length);
    return p + BLOCK;
}
static int compare_ids(const void *lhs, const void *rhs) {
    uint32_t a = *(const uint32_t *)lhs, b = *(const uint32_t *)rhs;
    return (a > b) - (a < b);
}

static SksePairResult sections(const uint8_t *data, size_t size, uint32_t uid,
                               uint32_t *ids, Span *s) {
    size_t pos = HEADER;
    s->first = s->end = size;
    for (uint32_t i = 0; i < s->plugins; ++i) {
        if (size - pos < BLOCK) return SKSE_PAIR_INVALID_DATA;
        size_t start = pos;
        ids[i] = u32(data + pos);
        uint32_t count = u32(data + pos + 4), length = u32(data + pos + 8);
        pos += BLOCK;
        if (length > size - pos || count > length / BLOCK) return SKSE_PAIR_INVALID_DATA;
        if (count > SKSE_PAIR_MAX_HEADERS - s->plugins - s->records) return SKSE_PAIR_LIMIT;
        size_t end = pos + length;
        for (uint32_t j = 0; j < count; ++j) {
            if (end - pos < BLOCK) return SKSE_PAIR_INVALID_DATA;
            uint32_t payload = u32(data + pos + 8);
            pos += BLOCK;
            if (payload > end - pos) return SKSE_PAIR_INVALID_DATA;
            pos += payload;
        }
        if (pos != end) return SKSE_PAIR_INVALID_DATA;
        s->records += count;
        if (ids[i] == uid) { s->first = start; s->end = end; s->owned_records = count; }
    }
    if (pos != size) return SKSE_PAIR_INVALID_DATA;
    if (s->plugins > 1) {
        qsort(ids, s->plugins, sizeof *ids, compare_ids);
        for (uint32_t i = 1; i < s->plugins; ++i)
            if (ids[i] == ids[i - 1]) return SKSE_PAIR_DUPLICATE_UID;
    }
    return SKSE_PAIR_OK;
}

static SksePairResult scan(const uint8_t *data, size_t size, uint32_t uid, Span *s) {
    if (size > SKSE_PAIR_MAX_COMPANION) return SKSE_PAIR_LIMIT;
    if (!data || size < HEADER || memcmp(data, "SKSE", 4) || !u32(data + 4)) return SKSE_PAIR_INVALID_DATA;
    if (u32(data + 4) != 1 || u32(data + 12) != 0x09200000u) return SKSE_PAIR_UNSUPPORTED;
    s->plugins = u32(data + 16);
    if (s->plugins > (size - HEADER) / BLOCK) return SKSE_PAIR_INVALID_DATA;
    if (s->plugins > SKSE_PAIR_MAX_HEADERS) return SKSE_PAIR_LIMIT;
    uint32_t *ids = s->plugins ? malloc(sizeof *ids * s->plugins) : NULL;
    if (s->plugins && !ids) return SKSE_PAIR_STORAGE;
    SksePairResult result = sections(data, size, uid, ids, s);
    free(ids);
    return result;
}

SksePairResult skse_records_validate(const void *data, size_t size) {
    Span s = {0};
    return scan(data, size, 0, &s);
}
SksePairResult skse_records_empty(SksePairImage *out) {
    uint8_t *p = calloc(1, HEADER);
    if (!p) return SKSE_PAIR_STORAGE;
    memcpy(p, "SKSE", 4); put32(p + 4, 1); put32(p + 8, 0x01070030u); put32(p + 12, 0x09200000u);
    *out = (SksePairImage){p, HEADER};
    return SKSE_PAIR_OK;
}
SksePairResult skse_records_owner(const SksePairImage *image, uint32_t uid, SkseOwnerView *out) {
    *out = (SkseOwnerView){0};
    Span s = {0};
    SksePairResult result = scan(image->data, image->size, uid, &s);
    if (result != SKSE_PAIR_OK) return result;
    if (s.first == image->size) return SKSE_PAIR_OWNER_MISSING;
    *out = (SkseOwnerView){image->data + s.first + BLOCK, s.owned_records};
    return SKSE_PAIR_OK;
}
SksePairResult skse_records_each(const SkseOwnerView *owner, SksePairVisitor visitor, void *context) {
    const uint8_t *p = owner->data;
    for (uint32_t i = 0; i < owner->count; ++i) {
        SksePairRecord row = {u32(p), u32(p + 4), u32(p + 8), p + BLOCK};
        if (!visitor(context, &row)) return SKSE_PAIR_STOPPED;
        p += BLOCK + row.length;
    }
    return SKSE_PAIR_OK;
}
SksePairResult skse_records_visit(const SksePairImage *image, uint32_t uid,
                                  SksePairVisitor visitor, void *context) {
    SkseOwnerView owner;
    SksePairResult result = skse_records_owner(image, uid, &owner);
    return result == SKSE_PAIR_OK ? skse_records_each(&owner, visitor, context) : result;
}

SksePairResult skse_records_replace(SksePairImage *image, uint32_t uid,
                                    const SksePairRecord *records, size_t count) {
    if (!uid || (count && !records)) return SKSE_PAIR_ARGUMENT;
    if (count > SKSE_PAIR_MAX_HEADERS - 1) return SKSE_PAIR_LIMIT;
    size_t section_size = count ? BLOCK : 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].length && !records[i].data) return SKSE_PAIR_ARGUMENT;
        if (section_size + BLOCK + (uint64_t)records[i].length > SKSE_PAIR_MAX_COMPANION) return SKSE_PAIR_LIMIT;
        section_size += BLOCK + records[i].length;
    }
    Span s = {0};
    SksePairResult result = scan(image->data, image->size, uid, &s);
    if (result != SKSE_PAIR_OK) return result;
    uint32_t plugins = s.plugins - (s.first != image->size) + (count != 0);
    size_t size = image->size - (s.end - s.first) + section_size;
    if (size > SKSE_PAIR_MAX_COMPANION || plugins + s.records - s.owned_records + count > SKSE_PAIR_MAX_HEADERS)
        return SKSE_PAIR_LIMIT;
    if (!count && s.first == image->size) return SKSE_PAIR_OK;
    uint8_t *bytes = malloc(size);
    if (!bytes) return SKSE_PAIR_STORAGE;
    memcpy(bytes, image->data, s.first);
    put32(bytes + 16, plugins);
    uint8_t *p = bytes + s.first;
    if (count) {
        p = block(p, uid, (uint32_t)count, (uint32_t)(section_size - BLOCK));
        for (size_t i = 0; i < count; ++i) {
            p = block(p, records[i].type, records[i].version, records[i].length);
            if (records[i].length) memcpy(p, records[i].data, records[i].length);
            p += records[i].length;
        }
    }
    memcpy(p, image->data + s.end, image->size - s.end);
    free(image->data);
    *image = (SksePairImage){bytes, size};
    return SKSE_PAIR_OK;
}
