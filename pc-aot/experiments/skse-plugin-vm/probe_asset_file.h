/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PROBE_ASSET_FILE_H
#define SKYRIM_EXPERIMENT_SKSE_PROBE_ASSET_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* context points to the caller's open, read-only directory fd for the selected
 * MOD. path is a normalized UTF-8 relative path with '/' separators. On success
 * the caller owns *bytes, including for an empty file, and must free it. On
 * error the outputs are NULL and zero. Return values are Windows error numbers:
 * 2 file missing, 3 path missing, 5 denied, 8 no memory, 13 incomplete,
 * or 223 too large. This function never changes the directory fd's offset. */
uint32_t skse_probe_asset_read(void *context, const char *path, size_t max_size,
                               uint8_t **bytes, size_t *length);
/* Same snapshot contract, requiring one case-fold match per path component
 * even when the requested spelling also exists exactly. */
uint32_t skse_probe_asset_read_unique(void *context, const char *path, size_t max_size,
                                      uint8_t **bytes, size_t *length);
/* Snapshot up to limit bytes from the start of one uniquely named asset.
 * A larger regular file is allowed. The same descriptor, identity, and
 * post-read name checks apply as with the complete-file reader. */
uint32_t skse_probe_asset_read_prefix_unique(void *context, const char *path, size_t limit,
                                             uint8_t **bytes, size_t *length);
/* Get the selected file or directory's type without reading its contents. */
uint32_t skse_probe_asset_attributes(void *context, const char *path, uint32_t *attributes);

#ifdef __cplusplus
}
#endif
#endif
