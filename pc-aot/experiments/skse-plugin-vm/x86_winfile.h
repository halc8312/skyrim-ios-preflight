/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINFILE_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SKSE_WIN_PATH_UNITS = 260, SKSE_WIN_PATH_BYTES = 1024, SKSE_WIN_OPEN_FILES = 4,
    SKSE_WIN_OUTPUT_FILES = 4, SKSE_WIN_OUTPUT_DIRECTORIES = 16,
    SKSE_WIN_FILE_MAX_BYTES = 1024 * 1024, SKSE_WIN_ATTRIBUTE_REQUEST_LIMIT = 1024
};
#define SKSE_WIN_VIRTUAL_GAME_ROOT "C:\\Skyrim\\"
#define SKSE_WIN_VIRTUAL_GAME_EXE "TESV.exe"

typedef enum {
    SKSE_WIN_OPEN_FILE_A, SKSE_WIN_OPEN_FILE_W, SKSE_WIN_READ_FILE, SKSE_WIN_FILE_SIZE,
    SKSE_WIN_FILE_SIZE_EX, SKSE_WIN_CLOSE_FILE, SKSE_WIN_SEEK_FILE_EX, SKSE_WIN_FILE_TYPE,
    SKSE_WIN_ATTRIBUTES_A, SKSE_WIN_ATTRIBUTES_W, SKSE_WIN_SEEK_FILE, SKSE_WIN_WRITE_FILE,
    SKSE_WIN_SET_END_OF_FILE, SKSE_WIN_CREATE_DIRECTORY_W, SKSE_WIN_FILE_OPERATION_COUNT
} SkseWinFileOperation;

enum { SKSE_WIN_ATTRIBUTE_DIRECTORY = 0x10, SKSE_WIN_ATTRIBUTE_NORMAL = 0x80 };
enum { SKSE_WIN_FILE_READ = 1, SKSE_WIN_FILE_WRITE = 2 };
typedef enum {
    SKSE_WIN_CREATE_NEW = 1, SKSE_WIN_CREATE_ALWAYS, SKSE_WIN_OPEN_EXISTING,
    SKSE_WIN_OPEN_ALWAYS, SKSE_WIN_TRUNCATE_EXISTING
} SkseWinFileDisposition;

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

/* Installed only by a trusted host. An immutable file in the base source
 * always takes precedence over a profile input. The policy receives the
 * normalized virtual path and shares the input provider's context. */
typedef bool (*SkseWinProfileGrant)(void *context, const char *path);
typedef struct {
    SkseWinAssets input;
    SkseWinProfileGrant allow_write;
} SkseWinProfile;

typedef struct {
    uint8_t *bytes;
    char *path;
    size_t length, capacity;
    uint32_t profile_error;
    bool profile, profile_dirty;
} SkseWinFileData;

typedef struct {
    uint32_t token;
    SkseWinFileData snapshot;
    SkseWinFileData *data;
    uint64_t offset;
    unsigned access, share;
    bool profile_snapshot;
} SkseWinFile;

typedef struct {
    SkseWinAssets source;
    SkseWinProfile profile;
    size_t opened_bytes;
    unsigned requests, attribute_requests, directory_requests;
    unsigned request_window_start;
    unsigned directory_count;
    bool read_only;
    bool profile_aborted;
    SkseWinFile files[SKSE_WIN_OPEN_FILES];
    SkseWinFileData outputs[SKSE_WIN_OUTPUT_FILES];
    char *directories[SKSE_WIN_OUTPUT_DIRECTORIES];
} SkseWinFiles;

/* Normalize dot components lexically inside the virtual game root. No host
 * pathname is consulted. Empty results and escapes from that root fail. */
bool skse_win_asset_path(const uint16_t *units, size_t length, bool wide, char path[SKSE_WIN_PATH_BYTES]);
uint32_t skse_win_file_open(SkseWinFiles *files, const char *path, unsigned access, unsigned share,
                            SkseWinFileDisposition disposition, uint32_t *error);
bool skse_win_output_exists(SkseWinFiles *files, const char *path);

/* Paths are normalized by skse_win_asset_path before use. Resolve local
 * directories, output files and missing paths beneath them without host
 * access. Return false with outputs untouched when the asset provider
 * must resolve the path. On a local hit, error is zero and attributes
 * describe the entry. On a local miss, attributes is zero. */
bool skse_win_local_attributes(SkseWinFiles *files, const char *path,
                                uint32_t *attributes, uint32_t *error);

/* Query local state, then base and profile metadata. False means the guest's
 * external metadata budget has been exhausted. Ordinary Win32 errors are
 * returned in *error on true. Internal open/create checks are independent. */
bool skse_win_file_attributes(SkseWinFiles *files, const char *path, uint32_t *attributes, uint32_t *error);

/* Create one session-local directory. Its parent must already exist
 * in the session or in the read-only asset tree. Return zero or a Win32
 * error. All created paths are freed by skse_win_files_close. */
uint32_t skse_win_directory_create(SkseWinFiles *files, const char *path);

SkseWinFile *skse_win_file_lookup(SkseWinFiles *files, uint32_t token);
uint32_t skse_win_file_resize(SkseWinFile *file, uint64_t length);
/* Record the outcome of a nonempty write, including in-place writes with
 * unchanged length. A failed mutation cannot later be captured. */
void skse_win_file_note_write(SkseWinFile *, bool success);
void skse_win_file_close(SkseWinFile *file);
void skse_win_files_close(SkseWinFiles *files);

typedef struct {
    const SkseWinFileData *entries[SKSE_WIN_OUTPUT_FILES];
    size_t count;
} SkseWinProfileOutputs;

/* Return all eligible changed outputs or none. Report a failed guest callback
 * even when there is nothing to capture. It disables persistence for this
 * session. The host copies the borrowed list before running more guest code.
 * An empty successful list is a no-op. No host writes occur here. */
uint32_t skse_win_profile_closed_outputs(SkseWinFiles *, bool callback_succeeded, SkseWinProfileOutputs *);
/* Retire one published, closed profile output so later calls can reuse its
 * slot. The owning host must publish or otherwise consume the bytes first. */
bool skse_win_profile_release_closed(SkseWinFiles *, const SkseWinFileData *);
/* Refresh hosted callback budgets only when no profile handle is
 * outstanding. Session-local scratch handles keep their original tokens. */
bool skse_win_profile_begin_call(SkseWinFiles *);
#endif
