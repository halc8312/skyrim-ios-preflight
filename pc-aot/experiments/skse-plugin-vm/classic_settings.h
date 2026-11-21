/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_CLASSIC_SETTINGS_H
#define SKYRIM_CLASSIC_SETTINGS_H
#include "classic_scalar.h"
#include "plugin_trial.h"
#include "../skse-cosave/mod_history.h"

typedef struct SkseClassicSettings SkseClassicSettings;

typedef struct { uint8_t words[8], seen; } SkseSettingsBootstrap;

/* Collect the required input bytes during the existing executable digest
 * pass. The verified digest must come from that same read stream. */
void skse_classic_settings_scan_bootstrap(SkseSettingsBootstrap *, uint64_t offset, const void *, size_t size);
bool skse_classic_settings_copy_bootstrap(const SkseSettingsBootstrap *, const uint8_t digest[32], uint8_t words[8]);

typedef enum {
    SKSE_SETTINGS_GET_INT = 1, SKSE_SETTINGS_GET_FLOAT, SKSE_SETTINGS_GET_STRING,
    SKSE_SETTINGS_SET_INT, SKSE_SETTINGS_SET_FLOAT, SKSE_SETTINGS_SET_STRING, SKSE_SETTINGS_SAVE,
    SKSE_SETTINGS_LOAD, SKSE_SETTINGS_INT_LIST_CLEAR, SKSE_SETTINGS_INT_LIST_GET,
    SKSE_SETTINGS_INT_LIST_RESIZE, SKSE_SETTINGS_INT_LIST_SET, SKSE_SETTINGS_METHOD_LIMIT
} SkseSettingsMethod;

typedef enum {
    SKSE_SETTINGS_OK, SKSE_SETTINGS_ARGUMENT, SKSE_SETTINGS_VERSION,
    SKSE_SETTINGS_MEMORY, SKSE_SETTINGS_STARTUP, SKSE_SETTINGS_SIGNATURE,
    SKSE_SETTINGS_RETIRED, SKSE_SETTINGS_EXECUTION, SKSE_SETTINGS_STORAGE,
    SKSE_SETTINGS_PUBLISHED_UNSYNCED
} SkseSettingsStatus;

typedef struct {
    const uint8_t *dll, *pex;
    size_t dll_size, pex_size;
    int game_fd, settings_fd;
    uint8_t profile[16];
    /* The host extracts these two words from its verified LE executable. */
    uint8_t bootstrap[8];
    uint64_t epoch;
    uint32_t uptime_ms;
} SkseSettingsInputs;

/* One serialized owner for the lifetime of the settings scope. The selected
 * game view is read-only. The app-owned private directory and the game
 * descriptor are duplicated. Byte inputs are borrowed only for open. */
SkseSettingsStatus skse_classic_settings_open(const SkseSettingsInputs *, SkseClassicSettings **);
void skse_classic_settings_close(SkseClassicSettings *);

/* Optional launch preparation of a selected file using GetIntValue. The
 * preparation budget is larger than the gameplay budget. It cannot write. */
SkseSettingsStatus skse_classic_settings_prepare(SkseClassicSettings *, uint64_t epoch,
    const SkseClassicScalar arguments[3], uint32_t uptime_ms, SkseClassicScalarResult *, SkseX86Result *);

/* One pending call at a time. The result and ticket are assigned only on
 * success. Commit the ticket while the original receiver is still valid.
 * Discard retires the scope because the DLL may have changed its own state. */
SkseSettingsStatus skse_classic_settings_invoke(SkseClassicSettings *, uint64_t epoch, SkseSettingsMethod,
    const SkseClassicScalar *, size_t count, uint32_t uptime_ms, SkseClassicScalarResult *, uint64_t *ticket,
    SkseX86Result *);
SkseSettingsStatus skse_classic_settings_commit(SkseClassicSettings *, uint64_t epoch, uint64_t ticket);
void skse_classic_settings_discard(SkseClassicSettings *, uint64_t epoch, uint64_t ticket);

typedef enum {
    SKSE_STORAGE_KEYWORD_GET_INT, SKSE_STORAGE_KEYWORD_SET_INT, SKSE_STORAGE_KEYWORD_METHOD_LIMIT
} SkseStorageKeywordMethod;

/* Opt in for the verified Wearable Lanterns static Keyword for this scope.
 * The caller attests the current loaded owner and the original local Keyword.
 * The exact StorageUtil.pex bytes and the owner's name are borrowed only for
 * this call. An already enabled scope cannot change its owner index. */
SkseSettingsStatus skse_classic_storage_enable_keyword(SkseClassicSettings *, uint64_t epoch,
    const uint8_t *storage_pex, size_t pex_size, const SkseLoadedMod *owner);
/* The caller verifies the live Keyword and copies only its identity. The key
 * length includes NUL. int_bits is the GET default or SET value. Use the
 * same commit and discard calls as JsonUtil; neither native can write files. */
SkseSettingsStatus skse_classic_storage_invoke_keyword(SkseClassicSettings *, uint64_t epoch,
    SkseStorageKeywordMethod, const SkseClassicFormSnapshot *, const char *key, size_t key_size,
    uint32_t int_bits, uint32_t uptime_ms, SkseClassicScalarResult *, uint64_t *ticket, SkseX86Result *);

/* Exact PapyrusUtil 3.3 record callbacks in the same isolated session as its
 * natives. The owner serializes calls and supplies the current, frozen
 * Classic catalog for the verified game save or load. The callback sees
 * independent copies of the names and indices. */
SkseSettingsStatus skse_classic_storage_capture(SkseClassicSettings *, uint64_t epoch,
    const SkseLoadedMod *, size_t count, uint32_t uptime_ms, SksePluginRecordSet **out);
enum { SKSE_CLASSIC_STORAGE_UID = 0x424510a2 };
/* Reset the previous world's records after a verified load without a match. */
SkseSettingsStatus skse_classic_storage_reset(SkseClassicSettings *, uint64_t epoch,
    const SkseLoadedMod *, size_t count, uint32_t uptime_ms);
SkseSettingsStatus skse_classic_storage_restore(SkseClassicSettings *, uint64_t epoch,
    const SkseLoadedMod *, size_t count, uint32_t uptime_ms,
    const SksePluginRecordSet *, const SkseSerialResolvers *);
#endif
