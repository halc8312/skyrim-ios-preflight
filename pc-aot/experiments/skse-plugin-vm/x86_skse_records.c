/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_skse_records.h"
#include <stdlib.h>
#include <string.h>

static int append_bytes(SkseX86RecordStream *stream, const void *bytes, uint32_t length) {
    if (!length) return 1;
    if (!bytes || length > SKSE_X86_RECORD_BYTES - stream->used) return 0;

    uint32_t needed = stream->used + length;
    if (needed > stream->capacity) {
        uint32_t capacity = stream->capacity ? 2 * stream->capacity : 256;
        if (capacity < needed) capacity = needed;
        if (capacity > SKSE_X86_RECORD_BYTES) capacity = SKSE_X86_RECORD_BYTES;
        uint8_t *larger = realloc(stream->payload, capacity);
        if (!larger) return 0;
        stream->payload = larger;
        stream->capacity = capacity;
    }
    memcpy(stream->payload + stream->used, bytes, length);
    stream->used = needed;
    return 1;
}

int skse_x86_records_open(SkseX86RecordStream *stream, uint32_t type, uint32_t version) {
    return skse_x86_records_write(stream, type, version, NULL, 0);
}

int skse_x86_records_append(SkseX86RecordStream *stream, const void *bytes, uint32_t length) {
    if (!stream || stream->reading || !stream->count) return 0;
    return append_bytes(stream, bytes, length);
}

int skse_x86_records_write(SkseX86RecordStream *stream, uint32_t type, uint32_t version,
                           const void *bytes, uint32_t length) {
    if (!stream || stream->reading || stream->count >= SKSE_X86_RECORD_LIMIT) return 0;
    uint32_t offset = stream->used;
    if (!append_bytes(stream, bytes, length)) return 0;
    stream->records[stream->count].type = type;
    stream->records[stream->count].version = version;
    stream->records[stream->count].offset = offset;
    stream->count++;
    return 1;
}

uint32_t skse_x86_records_count(const SkseX86RecordStream *stream) {
    return stream ? stream->count : 0;
}

int skse_x86_records_copy(SkseX86RecordStream *destination, const SkseX86RecordStream *source) {
    if (!source || source->count > SKSE_X86_RECORD_LIMIT || !destination || source == destination ||
        destination->payload || destination->count || destination->reading) return 0;

    SkseX86RecordStream copy = {0};
    if (!append_bytes(&copy, source->payload, source->used)) return 0;
    memcpy(copy.records, source->records, source->count * sizeof(copy.records[0]));
    copy.count = source->count;
    *destination = copy;
    return 1;
}

int skse_x86_records_reset_reading(SkseX86RecordStream *stream) {
    if (!stream) return 0;
    stream->reading = 1;
    stream->next_record = 0;
    stream->read_position = stream->read_end = 0;
    return 1;
}

int skse_x86_records_next(SkseX86RecordStream *stream, uint32_t *type, uint32_t *version,
                          uint32_t *length) {
    if (!stream || !stream->reading || !type || !version || !length) return 0;
    if (stream->next_record == stream->count) {
        stream->read_position = stream->read_end;
        return 0;
    }
    uint32_t current = stream->next_record++;
    stream->read_position = stream->records[current].offset;
    stream->read_end = stream->next_record == stream->count ? stream->used
                       : stream->records[stream->next_record].offset;
    *type = stream->records[current].type;
    *version = stream->records[current].version;
    *length = stream->read_end - stream->read_position;
    return 1;
}

uint32_t skse_x86_records_read(SkseX86RecordStream *stream, void *out, uint32_t maximum) {
    if (!stream || !stream->reading || !out) return 0;
    uint32_t available = stream->read_end - stream->read_position;
    uint32_t length = maximum < available ? maximum : available;
    if (length) {
        memcpy(out, stream->payload + stream->read_position, length);
        stream->read_position += length;
    }
    return length;
}

void skse_x86_records_close(SkseX86RecordStream *stream) {
    if (!stream) return;
    free(stream->payload);
    *stream = (SkseX86RecordStream){0};
}
