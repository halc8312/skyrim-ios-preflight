/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_X86_SKSE_RECORDS_H
#define SKYRIM_EXPERIMENT_X86_SKSE_RECORDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The byte limit is the combined payload across the entire stream. */
enum { SKSE_X86_RECORD_LIMIT = 16, SKSE_X86_RECORD_BYTES = 64 * 1024 };

/* One caller owns and serializes all use of a zero-initialized stream. Its
 * fields are implementation state. Do not copy a live stream or pass its
 * storage as an input or output buffer. Buffers supplied here are host memory. */
typedef struct {
    uint8_t *payload;
    uint32_t used, capacity;
    struct { uint32_t type, version, offset; } records[SKSE_X86_RECORD_LIMIT];
    uint32_t count, next_record, read_position, read_end;
    int reading;
} SkseX86RecordStream;

/* Appending requires an open record. A zero-length buffer may be NULL.
 * Write creates and fills one record atomically. On a rejected write the
 * previous contents and current record are still available. */
int skse_x86_records_open(SkseX86RecordStream *stream, uint32_t type, uint32_t version);
int skse_x86_records_append(SkseX86RecordStream *stream, const void *bytes, uint32_t length);
int skse_x86_records_write(SkseX86RecordStream *stream, uint32_t type, uint32_t version,
                           const void *bytes, uint32_t length);
uint32_t skse_x86_records_count(const SkseX86RecordStream *stream);

/* Copy the records and payload into a zero-initialized destination with a fresh cursor.
 * On failure neither stream changes. The two streams own independent storage. */
int skse_x86_records_copy(SkseX86RecordStream *destination, const SkseX86RecordStream *source);

/* Seal writes and start or rewind reading. Next skips unread bytes even on
 * reaching the end; at the end its output values stay untouched. Reads are
 * clamped to the current record. Reset and next return one on success;
 * next returns zero on rejection or end of stream. */
int skse_x86_records_reset_reading(SkseX86RecordStream *stream);
int skse_x86_records_next(SkseX86RecordStream *stream, uint32_t *type, uint32_t *version,
                          uint32_t *length);
uint32_t skse_x86_records_read(SkseX86RecordStream *stream, void *out, uint32_t maximum);

/* Release the payload and return the stream to its empty writing state. */
void skse_x86_records_close(SkseX86RecordStream *stream);

#ifdef __cplusplus
}
#endif
#endif
