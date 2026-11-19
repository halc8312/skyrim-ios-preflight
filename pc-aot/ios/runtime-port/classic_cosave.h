/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of either file. The editor snapshots its source in memory. */
enum { SKYRIM_CLASSIC_COSAVE_MAX_BYTES = 64 * 1024 * 1024 };
typedef enum {
    SKYRIM_CLASSIC_OK, SKYRIM_CLASSIC_ARGUMENT, SKYRIM_CLASSIC_MISSING,
    SKYRIM_CLASSIC_UID_MISSING, SKYRIM_CLASSIC_EXISTS, SKYRIM_CLASSIC_UNSUPPORTED,
    SKYRIM_CLASSIC_TRUNCATED, SKYRIM_CLASSIC_MALFORMED, SKYRIM_CLASSIC_DUPLICATE_UID,
    SKYRIM_CLASSIC_LIMIT, SKYRIM_CLASSIC_IO, SKYRIM_CLASSIC_STOPPED
} SkyrimClassicResult;

typedef struct {
    uint32_t type, version, length;
    const void *data;
} SkyrimClassicRecord;
typedef int (*SkyrimClassicVisitor)(void *context, const SkyrimClassicRecord *record);

/* Both APIs accept .skse paths only, with case-insensitive extensions.
 * Validate the entire container before the first callback. Records and payloads
 * are borrowed during the call. A callback returning zero stops the visit. */
SkyrimClassicResult skyrim_classic_cosave_visit(const char *source, uint32_t uid,
                                                SkyrimClassicVisitor visitor, void *context);
/* Replace only this plugin's records. NULL source explicitly creates a new
 * Classic 1.7.3 container. Zero records omit the section, like the Classic
 * writer. UID 0 belongs to SKSE itself and may only be read. Other owners'
 * sections remain byte-for-byte unchanged, in their original order.
 * Destination must be new, including for retries. Publication requires
 * same-directory hard-link support. Abrupt process termination before
 * publication can leave a private temporary file; the destination stays absent. */
SkyrimClassicResult skyrim_classic_cosave_replace(const char *source, const char *destination,
                                                  uint32_t uid, const SkyrimClassicRecord *records,
                                                  size_t count);
const char *skyrim_classic_cosave_result(SkyrimClassicResult result);

#ifdef __cplusplus
}
#endif
