/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SkseProfileJsonStore SkseProfileJsonStore;
typedef struct SkseProfileJsonSnapshot SkseProfileJsonSnapshot;

typedef struct {
    uint8_t profile[16];   /* Stable identifier chosen by the application. */
    uint8_t owner[32];     /* SHA-256 identity of the authorized producer. */
} SkseProfileJsonIdentity;

enum {
    SKSE_PROFILE_JSON_MAX_FILE = 1024 * 1024,
    SKSE_PROFILE_JSON_MAX_TOTAL = 4 * SKSE_PROFILE_JSON_MAX_FILE,
    SKSE_PROFILE_JSON_MAX_FILES = 32,
};

typedef struct {
    SkseProfileJsonIdentity identity;
    const char *virtual_prefix;
    size_t file_bytes, total_bytes;
    unsigned files;
} SkseProfileJsonGrant;

typedef enum {
    SKSE_PROFILE_JSON_OK,
    SKSE_PROFILE_JSON_ARGUMENT,
    SKSE_PROFILE_JSON_SCOPE,
    SKSE_PROFILE_JSON_MISSING,
    SKSE_PROFILE_JSON_CONFLICT,
    SKSE_PROFILE_JSON_CHANGED,
    SKSE_PROFILE_JSON_LIMIT,
    SKSE_PROFILE_JSON_NO_MEMORY,
    SKSE_PROFILE_JSON_IO,
    SKSE_PROFILE_JSON_PUBLISHED_UNSYNCED,
    SKSE_PROFILE_JSON_ALREADY_PUBLISHED,
} SkseProfileJsonResult;

/* Duplicate an existing private app-owned directory. No directory is created
 * until publishing. The application owns the identities and access grants.
 * Calls may run concurrently within one process. Close after all use ends. */
SkseProfileJsonResult skse_profile_json_open(int app_dir, const SkseProfileJsonGrant *, SkseProfileJsonStore **);
void skse_profile_json_close(SkseProfileJsonStore *);
bool skse_profile_json_matches(const SkseProfileJsonStore *, const SkseProfileJsonIdentity *);

/* The normalized virtual prefix and its ancestors are mounted directories.
 * Other existing entries must resolve beneath the same private store. */
bool skse_profile_json_allows(const SkseProfileJsonStore *, const SkseProfileJsonIdentity *, const char *path);
SkseProfileJsonResult skse_profile_json_attributes(const SkseProfileJsonStore *, const SkseProfileJsonIdentity *,
                                                  const char *path, int *is_directory);

/* Copy closed virtual output bytes. The normalized ASCII relative JSON path
 * must be inside this store's granted prefix. The source identity is mandatory. */
SkseProfileJsonResult skse_profile_json_capture(const SkseProfileJsonStore *, const SkseProfileJsonIdentity *,
                                               const char *path, const uint8_t *bytes, size_t length,
                                               SkseProfileJsonSnapshot **);
void skse_profile_json_discard(SkseProfileJsonSnapshot *);

/* Only this still-open grant can publish its snapshots. Successful rename
 * consumes the publication even when the final directory sync later fails.
 * Before rename, failure leaves the previous JSON intact. */
SkseProfileJsonResult skse_profile_json_publish(const SkseProfileJsonStore *, SkseProfileJsonSnapshot *);

/* Return newly allocated bytes for the same identity and authorized path.
 * The caller frees them. On failure, the outputs are null and zero. */
SkseProfileJsonResult skse_profile_json_read(const SkseProfileJsonStore *, const SkseProfileJsonIdentity *,
                                            const char *path, size_t limit, uint8_t **bytes, size_t *length);
