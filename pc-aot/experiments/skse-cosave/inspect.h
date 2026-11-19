/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKSE_INSPECT_OK,
    SKSE_INSPECT_MISSING,
    SKSE_INSPECT_AMBIGUOUS,
    SKSE_INSPECT_UNSUPPORTED,
    SKSE_INSPECT_INVALID,
    SKSE_INSPECT_LIMIT,
    SKSE_INSPECT_UNSTABLE,
    SKSE_INSPECT_IO,
    SKSE_INSPECT_ARGUMENT,
    SKSE_INSPECT_BASE_MISSING
} SkseInspectStatus;
typedef struct {
    uint64_t size;
    uint32_t format_version, skse_version, runtime_version, owners, records;
} SkseInspectInfo;
/* The caller supplies a canonical absolute directory under the active profile
 * and one visible .ess filename. Matching folds ASCII case and preserves the
 * remaining filename bytes. Both names must be unique and refer to regular
 * files with a link count of one. No path component may be a symbolic link.
 * OK confirms the companion's record structure. Its opaque payloads and the
 * saved game's generation require separate verification. Only OK returns
 * counts. UNSUPPORTED retains the inspected version fields. All other
 * failures clear the complete result. */
SkseInspectStatus skse_inspect_companion(const char *save_directory, const char *ess_name,
                                         SkseInspectInfo *out);
#ifdef __cplusplus
}
#endif
