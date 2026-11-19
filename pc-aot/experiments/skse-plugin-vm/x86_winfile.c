/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winfile.h"
#include <stdlib.h>
#include <string.h>

enum {
    ACCESS_DENIED = 5, TOO_MANY_OPEN_FILES = 4, NOT_ENOUGH_MEMORY = 8, INVALID_DATA = 13,
    SHARING_VIOLATION = 32, FILE_TOO_LARGE = 223,
    MAX_OPEN_ATTEMPTS = 32, MAX_FILE_BYTES = 1024 * 1024, MAX_OPENED_BYTES = 4 * 1024 * 1024
};

static bool finish_component(const char *path, size_t first, size_t end) {
    if (first == end) return false;
    // Windows may remove trailing spaces and dots. Keep one unambiguous name.
    return path[end - 1] != ' ' && path[end - 1] != '.';
}

static size_t write_utf8(char *out, uint32_t value) {
    if (value < 0x80) {
        out[0] = (char)value;
        return 1;
    }
    if (value < 0x800) {
        out[0] = (char)(0xc0 | value >> 6);
        out[1] = (char)(0x80 | (value & 63));
        return 2;
    }
    if (value < 0x10000) {
        out[0] = (char)(0xe0 | value >> 12);
        out[1] = (char)(0x80 | (value >> 6 & 63));
        out[2] = (char)(0x80 | (value & 63));
        return 3;
    }
    out[0] = (char)(0xf0 | value >> 18);
    out[1] = (char)(0x80 | (value >> 12 & 63));
    out[2] = (char)(0x80 | (value >> 6 & 63));
    out[3] = (char)(0x80 | (value & 63));
    return 4;
}

bool skse_win_asset_path(const uint16_t *units, size_t length, bool wide, char path[SKSE_WIN_PATH_BYTES]) {
    if (length >= SKSE_WIN_PATH_UNITS) return false;
    size_t end = 0, start = 0;
    for (size_t i = 0; i < length; ++i) {
        uint32_t value = units[i];
        if (value == '/' || value == '\\') {
            if (!finish_component(path, start, end) || end + 1 >= SKSE_WIN_PATH_BYTES) return false;
            path[end++] = '/';
            start = end;
            continue;
        }
        if (value < 0x20 || value == 0x7f || value == ':' || value == '*' || value == '?' ||
            value == '"' || value == '<' || value == '>' || value == '|' ||
            (!wide && value > 0x7e)) return false;
        if (value >= 0xd800 && value <= 0xdfff) {
            if (value >= 0xdc00 || i + 1 == length || units[i + 1] < 0xdc00 || units[i + 1] > 0xdfff)
                return false;
            value = 0x10000 + ((value - 0xd800) << 10) + (units[++i] - 0xdc00);
        }
        char encoded[4];
        size_t count = write_utf8(encoded, value);
        if (count >= SKSE_WIN_PATH_BYTES - end) return false;
        for (size_t j = 0; j < count; ++j) path[end++] = encoded[j];
    }
    if (!finish_component(path, start, end)) return false;
    path[end] = '\0';
    return true;
}

static unsigned lower_ascii(unsigned value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static bool same_path(const char *first, const char *second) {
    while (*first && lower_ascii((uint8_t)*first) == lower_ascii((uint8_t)*second)) {
        ++first;
        ++second;
    }
    return *first == *second;
}

uint32_t skse_win_file_open(SkseWinFiles *files, const char *path, bool shared, uint32_t *error) {
    if (!files->source.read) {
        *error = ACCESS_DENIED;
        return UINT32_MAX;
    }
    SkseWinFile *slot = NULL;
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i) {
        SkseWinFile *current = &files->files[i];
        if (current->token && (!shared || !current->shared) && same_path(current->path, path)) {
            *error = SHARING_VIOLATION;
            return UINT32_MAX;
        }
        if (!slot && !current->token) slot = current;
    }
    if (!slot || files->requests == MAX_OPEN_ATTEMPTS) {
        *error = TOO_MANY_OPEN_FILES;
        return UINT32_MAX;
    }
    size_t limit = MAX_OPENED_BYTES - files->opened_bytes;
    if (limit > MAX_FILE_BYTES) limit = MAX_FILE_BYTES;
    ++files->requests;
    uint8_t *bytes = NULL;
    size_t length = 0;
    uint32_t status = files->source.read(files->source.context, path, limit, &bytes, &length);
    if (!status && length > limit) status = FILE_TOO_LARGE;
    if (!status && length && !bytes) status = INVALID_DATA;
    if (status) {
        free(bytes);
        *error = status;
        return UINT32_MAX;
    }
    const size_t path_bytes = strlen(path) + 1;
    char *path_copy = malloc(path_bytes);
    if (!path_copy) {
        free(bytes);
        *error = NOT_ENOUGH_MEMORY;
        return UINT32_MAX;
    }
    memcpy(path_copy, path, path_bytes);
    files->opened_bytes += length;
    uint32_t token = UINT32_C(0xe3000000) + files->requests;
    *slot = (SkseWinFile){.token = token, .bytes = bytes, .path = path_copy, .length = length, .shared = shared};
    return token;
}

SkseWinFile *skse_win_file_lookup(SkseWinFiles *files, uint32_t token) {
    if (!token) return NULL;
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i)
        if (files->files[i].token == token) return &files->files[i];
    return NULL;
}

void skse_win_file_close(SkseWinFile *file) {
    free(file->bytes);
    free(file->path);
    *file = (SkseWinFile){0};
}

void skse_win_files_close(SkseWinFiles *files) {
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i) skse_win_file_close(&files->files[i]);
}
