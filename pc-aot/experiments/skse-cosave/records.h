/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "generation.h"

typedef struct { uint8_t *data; size_t size; } SksePairImage;
/* Return one after copying exactly n bytes at offset. The caller bounds
 * the source to size and checks its version around the complete scan.
 * Valid containers return counts. Unsupported versions return only header
 * fields. Other failures leave the summary zeroed. */
typedef int (*SkseRecordsReadAt)(void *context, size_t offset, void *out, size_t n);
typedef struct { uint32_t format_version, skse_version, runtime_version, owners, records; } SkseRecordsSummary;
SksePairResult skse_records_inspect(size_t size, SkseRecordsReadAt read_at, void *context,
                                    SkseRecordsSummary *out);
/* A bounded internal transaction edits a validated stage. Errors are sticky
 * until drop. The editor publishes its complete image only on success. */
typedef SksePairResult (*SkseStageEditor)(SksePairImage *image, void *context);
SksePairResult skse_pair_edit_image(SksePairStage *stage, SkseStageEditor editor, void *context);
/* Borrowed only while the validated, immutable image backing it is alive. */
typedef struct { const uint8_t *data; uint32_t count; } SkseOwnerView;
SksePairResult skse_records_owner(const SksePairImage *image, uint32_t uid, SkseOwnerView *out);
SksePairResult skse_records_each(const SkseOwnerView *owner, SksePairVisitor visitor, void *context);
SksePairResult skse_records_validate(const void *data, size_t size);
SksePairResult skse_records_empty(SksePairImage *out);
SksePairResult skse_records_replace(SksePairImage *image, uint32_t uid,
                                    const SksePairRecord *records, size_t count);
/* For trusted Classic runtime records. The ordinary plugin writer rejects UID 0. */
SksePairResult skse_records_replace_core(SksePairImage *image, const SksePairRecord *records, size_t count);
SksePairResult skse_records_visit(const SksePairImage *image, uint32_t uid,
                                  SksePairVisitor visitor, void *context);
