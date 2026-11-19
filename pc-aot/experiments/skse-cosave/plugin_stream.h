/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "plugin_records.h"
#include "records.h"

typedef struct { uint8_t *data; size_t used, capacity; uint32_t chunks; } SkseEncodedRecords;
typedef struct { size_t offset, length; uint32_t count; int includes, existed; } SkseNewSection;
struct SkseRecordWriter {
    SkseEncodedRecords *all;
    SkseNewSection *section;
    uint32_t uid;
    size_t header;
    SksePairResult error;
};
struct SkseRecordReader {
    SkseOwnerView owner;
    const uint8_t *payload;
    uint32_t left;
    const SkseModMap *map;
    SksePairResult error;
};
void skse_stream_put32(uint8_t *p, uint32_t value);
