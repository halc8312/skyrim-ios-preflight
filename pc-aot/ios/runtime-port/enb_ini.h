/* SPDX-License-Identifier: GPL-3.0-only
 * enbseries.ini (and enblocal.ini) reader for the ENB-compatible post-processing
 * (docs/ENB_PLAN.md). Read-only, bounded, never executes anything. Sections and
 * keys compare without case; a later duplicate wins, as in a Windows profile read.
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ENB_INI_MAX_BYTES = 1 << 20, ENB_INI_MAX_ENTRIES = 16384, ENB_INI_NAME = 64, ENB_INI_VALUE = 128 };
enum { ENB_INI_ISSUE_TRUNCATED_LINE = 1, ENB_INI_ISSUE_TOO_MANY = 2, ENB_INI_ISSUE_TOO_LARGE = 4 };

typedef struct { char section[ENB_INI_NAME], key[ENB_INI_NAME], value[ENB_INI_VALUE]; } EnbIniEntry;
typedef struct { EnbIniEntry *entries; size_t count; unsigned issues; } EnbIni;

/* text needs no NUL. Returns 0, or ENOMEM / EINVAL. issues reports what was cut. */
int enb_ini_parse(const char *text, size_t length, EnbIni *out);
void enb_ini_free(EnbIni *);
/* Value text, or NULL when the key is absent. */
const char *enb_ini_get(const EnbIni *, const char *section, const char *key);
/* ENB writes true/false; 1/0 and yes/no also appear. Returns def when absent or unreadable. */
int enb_ini_bool(const EnbIni *, const char *section, const char *key, int def);
float enb_ini_float(const EnbIni *, const char *section, const char *key, float def);
long enb_ini_int(const EnbIni *, const char *section, const char *key, long def);
/* Read a file without following links, at most ENB_INI_MAX_BYTES. */
int enb_ini_read_file(const char *path, EnbIni *out);

#ifdef __cplusplus
}
#endif
