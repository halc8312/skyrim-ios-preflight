/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winenv.h"
#include "x86_wintext.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int ordinal_upper(uint8_t byte) {
    return byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
}

static int compare_name(const SkseWinEnvEntry *entry, const uint8_t *name, size_t length) {
    const size_t common = entry->name_units < length ? entry->name_units : length;
    for (size_t i = 0; i < common; ++i) {
        const int difference = ordinal_upper(name[i]) - ordinal_upper(entry->utf16le[i * 2]);
        if (difference) return difference;
    }
    return (length > entry->name_units) - (length < entry->name_units);
}

uint32_t skse_win_env_set(SkseWinEnv *env, const uint8_t *name, size_t name_bytes,
                          const uint8_t *value, size_t value_bytes) {
    if (!env || !name || !name_bytes || (!value && value_bytes)) return SKSE_WIN_ENV_INVALID_PARAMETER;
    if (name_bytes > SKSE_WIN_ENV_MAX_NAME_BYTES || value_bytes > SKSE_WIN_ENV_MAX_VALUE_BYTES)
        return SKSE_WIN_ENV_NOT_SUPPORTED;
    for (size_t i = 0; i < name_bytes; ++i) {
        if (!name[i] || (i && name[i] == '=')) return SKSE_WIN_ENV_INVALID_PARAMETER;
        if (name[i] > 0x7f) return SKSE_WIN_ENV_NOT_SUPPORTED;
    }
    if (value_bytes && memchr(value, 0, value_bytes)) return SKSE_WIN_ENV_INVALID_PARAMETER;

    size_t index = 0;
    int order = 1;
    while (index < env->count && (order = compare_name(&env->entries[index], name, name_bytes)) > 0)
        ++index;
    const bool exists = index < env->count && order == 0;
    SkseWinEnvEntry *entry = &env->entries[index];
    if (!value) {
        if (!exists) return 0;
        env->entry_units -= entry->units;
        free(entry->utf16le);
        --env->count;
        if (index < env->count) memmove(entry, entry + 1, (env->count - index) * sizeof *entry);
        env->entries[env->count] = (SkseWinEnvEntry){0};
        return 0;
    }

    const size_t units = name_bytes + value_bytes + 2;
    const size_t total_units = env->entry_units - (exists ? entry->units : 0) + units;
    if ((!exists && env->count == SKSE_WIN_ENV_MAX_ENTRIES) || total_units >= SKSE_WIN_ENV_MAX_BLOCK_UNITS)
        return SKSE_WIN_ENV_NO_MEMORY;
    uint8_t *bytes = calloc(units, 2);
    if (!bytes) return SKSE_WIN_ENV_NO_MEMORY;
    if (exists) {
        memcpy(bytes, entry->utf16le, (name_bytes + 1) * 2);
    } else {
        for (size_t i = 0; i < name_bytes; ++i) bytes[i * 2] = name[i];
        bytes[name_bytes * 2] = '=';
    }
    if (value_bytes) {
        size_t written;
        const uint32_t error = skse_win_multi_to_wide(SKSE_WIN_CP_1252, 0, value, value_bytes,
                                                      bytes + (name_bytes + 1) * 2, value_bytes, &written);
        if (error || written != value_bytes) {
            free(bytes);
            return error ? error : SKSE_WIN_ENV_NOT_SUPPORTED;
        }
    }
    if (exists) {
        free(entry->utf16le);
    } else {
        if (index < env->count) memmove(entry + 1, entry, (env->count - index) * sizeof *entry);
        ++env->count;
    }
    *entry = (SkseWinEnvEntry){.utf16le = bytes, .units = units, .name_units = name_bytes};
    env->entry_units = total_units;
    return 0;
}

uint32_t skse_win_env_snapshot_w(const SkseWinEnv *env, uint8_t *utf16le, size_t capacity_units,
                                 size_t *written_units) {
    if (!written_units) return SKSE_WIN_ENV_INVALID_PARAMETER;
    *written_units = 0;
    if (!env || (capacity_units && !utf16le)) return SKSE_WIN_ENV_INVALID_PARAMETER;
    const size_t units = env->count ? env->entry_units + 1 : 2;
    if (capacity_units) {
        if (capacity_units < units) return SKSE_WIN_ENV_INSUFFICIENT_BUFFER;
        size_t offset = 0;
        for (size_t i = 0; i < env->count; ++i) {
            const SkseWinEnvEntry *entry = &env->entries[i];
            const size_t bytes = entry->units * 2;
            memcpy(utf16le + offset, entry->utf16le, bytes);
            offset += bytes;
        }
        memset(utf16le + offset, 0, units * 2 - offset);
    }
    *written_units = units;
    return 0;
}

void skse_win_env_close(SkseWinEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->count; ++i) free(env->entries[i].utf16le);
    *env = (SkseWinEnv){0};
}
