/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_world_seam.h"

#include <string.h>

enum { WORLD_QUEUE_RETURN = 0x004dfddd, ARRAY_FIELDS_BYTES = 12, RECORD_FIELDS_BYTES = 17 };

typedef struct {
    uint32_t actor_handle, title, distance;
    uint16_t length, capacity;
    uint8_t force;
} Chatter;

static bool read_range(const SkyrimFuzLiftMemory *memory, uint32_t address, void *result, size_t count) {
    return memory && memory->read && address && count && (uint64_t)address + count - 1 <= UINT32_MAX &&
        memory->read(memory->context, address, result, count);
}

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16);
}

static bool read_chatter(const SkyrimFuzLiftMemory *memory, uint32_t address, Chatter *record) {
    uint8_t data[RECORD_FIELDS_BYTES];
    if (!read_range(memory, address, data, sizeof data)) return false;
    *record = (Chatter){
        .actor_handle = u32(data), .title = u32(data + 4), .length = u16(data + 8),
        .capacity = u16(data + 10), .distance = u32(data + 12), .force = data[16],
    };
    return true;
}

static bool valid_title(const Chatter *record) {
    return record->title && record->length && record->length < record->capacity &&
        record->length < SKYRIM_FUZ_RESPONSE_TEXT_BYTES;
}

static bool array_span(const SkyrimFuzLiftMemory *memory, uint32_t array, uint32_t *data, uint32_t *count) {
    uint8_t fields[ARRAY_FIELDS_BYTES];
    if (!read_range(memory, array, fields, sizeof fields)) return false;
    *data = u32(fields);
    *count = u32(fields + 8);
    return *data && *count <= u32(fields + 4) &&
        (uint64_t)*data + (uint64_t)*count * SKYRIM_FUZ_WORLD_RECORD_BYTES <= UINT64_C(0x100000000);
}

SkyrimFuzWorldScope skyrim_fuz_world_capture(const SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                                            uint32_t response, uint32_t submitted_voice) {
    uint8_t field[4];
    if (!tracker->count || !submitted_voice || !read_range(memory, response, field, sizeof field))
        return (SkyrimFuzWorldScope){0};
    uint32_t title = u32(field);
    return (SkyrimFuzWorldScope){
        .response = response, .submitted_voice = submitted_voice, .queued_title = title,
        .serial = skyrim_fuz_substitution_serial(tracker, memory, response, submitted_voice, title),
    };
}

static bool still_selected(const SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                           const SkyrimFuzWorldScope *scope) {
    return scope && scope->serial && scope->serial == skyrim_fuz_substitution_serial(
        tracker, memory, scope->response, scope->submitted_voice, scope->queued_title);
}

bool skyrim_fuz_world_admit(const SkyrimFuzMenuTracker *tracker, const SkyrimFuzLiftMemory *memory,
                            const SkyrimFuzWorldScope *scope, uint32_t address, uint32_t title) {
    return address == WORLD_QUEUE_RETURN && scope && scope->queued_title == title && still_selected(tracker, memory, scope);
}

void skyrim_fuz_world_release_title(SkyrimFuzWorldTracker *tracker, uint32_t title) {
    for (size_t i = 0; i < tracker->count; ++i) {
        if (tracker->active[i].owned_title != title) continue;
        tracker->active[i] = tracker->active[--tracker->count];
        return;
    }
}

static bool same_copy_fields(const Chatter *a, const Chatter *b) {
    return a->title != b->title && a->actor_handle == b->actor_handle && a->distance == b->distance &&
        a->force == b->force && a->length == b->length;
}

void skyrim_fuz_world_after_copy(SkyrimFuzWorldTracker *tracker, const SkyrimFuzMenuTracker *responses,
                                const SkyrimFuzLiftMemory *memory, uint32_t array, uint32_t index,
                                uint32_t source, const SkyrimFuzWorldScope *scope) {
    uint32_t data, count;
    Chatter final;
    if (!array_span(memory, array, &data, &count) || index >= count ||
        !read_chatter(memory, data + index * SKYRIM_FUZ_WORLD_RECORD_BYTES, &final)) return;

    // Invalidate any previous identity before accepting an allocation reused by an ordinary line.
    skyrim_fuz_world_release_title(tracker, final.title);
    if (!valid_title(&final) || tracker->count == SKYRIM_FUZ_MAX_CHATTER_MARKS ||
        !still_selected(responses, memory, scope)) return;
    Chatter original;
    if (!read_chatter(memory, source, &original) || !valid_title(&original) || !same_copy_fields(&original, &final)) return;

    // The temporary record must also be the copy of the exact submitted response.
    const size_t size = (size_t)original.length + 1;
    char response[SKYRIM_FUZ_RESPONSE_TEXT_BYTES], copied[SKYRIM_FUZ_RESPONSE_TEXT_BYTES];
    if (!skyrim_fuz_copy_response_text(memory, scope->response, response) || strlen(response) != original.length ||
        !read_range(memory, original.title, copied, size) || memcmp(response, copied, size) ||
        !read_range(memory, final.title, copied, size) || memcmp(response, copied, size)) return;
    tracker->active[tracker->count++] = (SkyrimFuzChatterMark){
        .array = array, .owned_title = final.title, .actor_handle = final.actor_handle,
        .length = final.length, .capacity = final.capacity,
    };
}

bool skyrim_fuz_world_general(const SkyrimFuzWorldTracker *tracker, const SkyrimFuzLiftMemory *memory,
                               uint32_t array, uint32_t record) {
    if (!tracker->count) return false;
    uint32_t data, count;
    Chatter current;
    if (!array_span(memory, array, &data, &count) || record < data ||
        (record - data) % SKYRIM_FUZ_WORLD_RECORD_BYTES ||
        (record - data) / SKYRIM_FUZ_WORLD_RECORD_BYTES >= count ||
        !read_chatter(memory, record, &current) || !valid_title(&current)) return false;
    for (size_t i = 0; i < tracker->count; ++i) {
        const SkyrimFuzChatterMark *mark = &tracker->active[i];
        if (mark->array == array && mark->owned_title == current.title && mark->actor_handle == current.actor_handle &&
            mark->length == current.length && mark->capacity == current.capacity) return true;
    }
    return false;
}
