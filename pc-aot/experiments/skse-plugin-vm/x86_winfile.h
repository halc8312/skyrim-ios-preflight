/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINFILE_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SKSE_WIN_PATH_UNITS = 260, SKSE_WIN_PATH_BYTES = 1024, SKSE_WIN_OPEN_FILES = 4
};

typedef enum {
    SKSE_WIN_OPEN_FILE_A, SKSE_WIN_OPEN_FILE_W, SKSE_WIN_READ_FILE, SKSE_WIN_FILE_SIZE,
    SKSE_WIN_FILE_SIZE_EX, SKSE_WIN_CLOSE_FILE, SKSE_WIN_SEEK_FILE_EX, SKSE_WIN_FILE_TYPE,
    SKSE_WIN_ATTRIBUTES_A, SKSE_WIN_ATTRIBUTES_W, SKSE_WIN_SEEK_FILE, SKSE_WIN_FILE_OPERATION_COUNT
} SkseWinFileOperation;

enum { SKSE_WIN_ATTRIBUTE_DIRECTORY = 0x10, SKSE_WIN_ATTRIBUTE_NORMAL = 0x80 };

static inline unsigned skse_win_ascii_lower(unsigned value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static inline bool skse_win_asset_name_equal(const char *a, const char *b) {
    while (*a && skse_win_ascii_lower((uint8_t)*a) == skse_win_ascii_lower((uint8_t)*b)) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Return zero for an immutable snapshot allocated with malloc. On failure
 * return a Win32 error code. The lifetime of the source itself is call-only. */
typedef uint32_t (*SkseWinReadAsset)(void *context, const char *path, size_t limit, uint8_t **bytes, size_t *length);
typedef uint32_t (*SkseWinAssetAttributes)(void *context, const char *path, uint32_t *attributes);

typedef struct {
    SkseWinReadAsset read;
    SkseWinAssetAttributes attributes;
    void *context;
} SkseWinAssets;

typedef struct {
    uint32_t token;
    uint8_t *bytes;
    char *path;
    size_t length;
    uint64_t offset;
    bool shared;
} SkseWinFile;

typedef struct {
    SkseWinAssets source;
    size_t opened_bytes;
    unsigned requests, attribute_requests;
    SkseWinFile files[SKSE_WIN_OPEN_FILES];
} SkseWinFiles;

bool skse_win_asset_path(const uint16_t *units, size_t length, bool wide, char path[SKSE_WIN_PATH_BYTES]);
uint32_t skse_win_file_open(SkseWinFiles *files, const char *path, bool shared, uint32_t *error);
SkseWinFile *skse_win_file_lookup(SkseWinFiles *files, uint32_t token);
void skse_win_file_close(SkseWinFile *file);
void skse_win_files_close(SkseWinFiles *files);
#endif
