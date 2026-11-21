/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Fuz Ro D-oh LE 6.1 settings from the selected profile's Data folders
 * (SKSE/Plugins/Fuz Ro D'oh.ini, [General]). Read-only. A missing file or key
 * keeps the author's default; an unusable value falls back to it and is
 * reported in `issues`. */
enum { SKYRIM_FUZ_DEFAULT_WORDS_PER_SECOND = 2, SKYRIM_FUZ_DEFAULT_SKIP_EMPTY = 1,
       SKYRIM_FUZ_STOCK_CLIPS = 10 };
enum { SKYRIM_FUZ_ISSUE_UNREADABLE = 1, SKYRIM_FUZ_ISSUE_WORDS_VALUE = 2, SKYRIM_FUZ_ISSUE_SKIP_VALUE = 4 };

typedef struct {
    int words_per_second; /* at least 1; the author treats 0 or less as 2 */
    int skip_empty_responses; /* 0 or 1; any nonzero INI value means 1, as in the original */
    unsigned issues;      /* SKYRIM_FUZ_ISSUE_* bits */
    int found_file;
} SkyrimFuzSettings;

/* roots: original Data, then packages in overwrite order (later wins). */
int skyrim_fuz_read_settings(const char *const *roots, size_t count, SkyrimFuzSettings *out);
/* Parse INI text (no NUL needed). Exposed for tests. */
void skyrim_fuz_parse_settings(const char *text, size_t length, SkyrimFuzSettings *out);
/* Data-relative stock clip names Sound/Voice/Fuz Ro Doh/Stock_N.xwm for N = 1..10. */
const char *skyrim_fuz_stock_path(unsigned seconds);
#ifdef __cplusplus
}
#endif
