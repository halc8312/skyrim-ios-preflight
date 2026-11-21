/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_CLASSIC_SETTINGS_H
#define SKYRIM_CLASSIC_SETTINGS_H
#include "classic_scalar.h"

typedef struct SkseClassicSettings SkseClassicSettings;

typedef struct { uint8_t words[8], seen; } SkseSettingsBootstrap;

/* Collect the required input bytes during the existing executable digest
 * pass. The verified digest must come from that same read stream. */
void skse_classic_settings_scan_bootstrap(SkseSettingsBootstrap *, uint64_t offset, const void *, size_t size);
bool skse_classic_settings_copy_bootstrap(const SkseSettingsBootstrap *, const uint8_t digest[32], uint8_t words[8]);

typedef enum {
    SKSE_SETTINGS_GET_INT = 1, SKSE_SETTINGS_GET_FLOAT, SKSE_SETTINGS_GET_STRING,
    SKSE_SETTINGS_SET_INT, SKSE_SETTINGS_SET_FLOAT, SKSE_SETTINGS_SET_STRING, SKSE_SETTINGS_SAVE
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
#endif
