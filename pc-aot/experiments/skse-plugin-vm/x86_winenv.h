/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINENV_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINENV_H

#include <stddef.h>
#include <stdint.h>

enum {
    SKSE_WIN_ENV_MAX_ENTRIES = 32, SKSE_WIN_ENV_MAX_NAME_BYTES = 127,
    SKSE_WIN_ENV_MAX_VALUE_BYTES = 4095, SKSE_WIN_ENV_MAX_BLOCK_UNITS = 8192,
    SKSE_WIN_ENV_NO_MEMORY = 8, SKSE_WIN_ENV_NOT_SUPPORTED = 50,
    SKSE_WIN_ENV_INVALID_PARAMETER = 87, SKSE_WIN_ENV_INSUFFICIENT_BUFFER = 122
};

typedef struct {
    uint8_t *utf16le;
    size_t units, name_units;
} SkseWinEnvEntry;

/* One independent guest process owns a zero-initialized table and serializes
 * access. Entries own their UTF-16LE bytes. Close the table at process teardown. */
typedef struct {
    SkseWinEnvEntry entries[SKSE_WIN_ENV_MAX_ENTRIES];
    size_t count, entry_units;
} SkseWinEnv;

/* Input spans exclude the terminator. Names are ASCII, ordinal and
 * case-insensitive. A leading '=' is allowed for hidden Windows entries.
 * Values use Windows-1252. A non-NULL zero-length value sets an empty value.
 * A NULL value with zero length deletes the variable. Returns a Win32 error
 * code or zero. A failed operation leaves the table unchanged. */
uint32_t skse_win_env_set(SkseWinEnv *env, const uint8_t *name, size_t name_bytes,
                          const uint8_t *value, size_t value_bytes);

/* A zero destination capacity queries the UTF-16 unit count, including the
 * double NUL. The caller provides disjoint storage of capacity_units * 2 bytes
 * for a snapshot. An invalid or undersized destination leaves bytes untouched
 * and sets written_units to zero when supplied. */
uint32_t skse_win_env_snapshot_w(const SkseWinEnv *env, uint8_t *utf16le, size_t capacity_units,
                                 size_t *written_units);

void skse_win_env_close(SkseWinEnv *env);

#endif
