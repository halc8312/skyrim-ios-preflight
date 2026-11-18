/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 7z / RAR / RAR5 through libarchive. Status values are the
 * SKYRIM_ZIP_* codes from mod_archive.h; the same path normalization,
 * link/encryption refusal, duplicate and size limits apply. */
typedef struct { char **paths; size_t count, skipped; uint64_t total; } SkyrimArchiveList;
int skyrim_archive_list(const char *path, SkyrimArchiveList *list);
void skyrim_archive_list_free(SkyrimArchiveList *list);
/* Extract regular files under prefix ("" for all) to dest_root, prefix
 * removed. Existing files are never replaced. */
int skyrim_archive_extract(const char *path, const char *prefix, int skip_fomod, const char *dest_root,
                           size_t *files, uint64_t *bytes);
#ifdef __cplusplus
}
#endif
