/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_RUNTIME_SAVE_H
#define CLASSIC_RUNTIME_SAVE_H

#include "save_publish.h"

struct CPU;
enum { CLASSIC_RUNTIME_PATH_BYTES = 1024 };

typedef struct {
    ClassicSavePublish file;
    const struct CPU *owner;
    uint64_t token;
    char directory[CLASSIC_RUNTIME_PATH_BYTES];
    uint32_t guest_handle;
    size_t last_requested;
    ssize_t last_written;
    unsigned writes, durability_stub_calls;
    struct stat before_close;
    int close_result, close_error, move_error;
    bool opening, flush_seen, os_close_seen, have_before_close, crt_close_seen;
    bool final_scope, move_seen, renamed;
} ClassicRuntimeSave;

/* One opted-in, single-owner observation. The caller keeps the state and its
 * authorized directory alive until end, on the same host thread. */
bool classic_runtime_save_begin(ClassicRuntimeSave *, const struct CPU *, int directory,
                               const char *mapped_directory, const char *final_leaf);
void classic_runtime_save_end(ClassicRuntimeSave *);
int classic_runtime_save_reader(const ClassicRuntimeSave *);

/* Lifted boundaries around the fresh stream creation, BSFile destructor and
 * final move. These have no effect when no observation is active. */
void classic_runtime_save_open_scope(const struct CPU *, bool opening);
void classic_runtime_save_flush_result(const struct CPU *, bool success);
void classic_runtime_save_crt_close_result(const struct CPU *, bool success);
void classic_runtime_save_final_scope(const struct CPU *);
void classic_runtime_save_crt_move_result(const struct CPU *, bool success);

/* Exact staged host wrapper observations. The token is copied before an I/O
 * call, allowing the wrapper to avoid reading GFile after that call. */
uint64_t classic_runtime_saved_opened(const struct CPU *, const char *mapped, int fd, bool shared);
void classic_runtime_saved_bound(const struct CPU *, uint64_t token, uint32_t handle);
bool classic_runtime_saved_io(const struct CPU *, uint64_t token);
void classic_runtime_saved_written(const struct CPU *, uint64_t token, int fd, size_t requested, ssize_t actual);
void classic_runtime_saved_closed(const struct CPU *, uint64_t token, uint32_t handle, int fd,
                                  const struct stat *before, int result, int error);
void classic_runtime_saved_flush_stub(const struct CPU *);
bool classic_runtime_saved_move_begin(const struct CPU *, const char *from, const char *to);
void classic_runtime_saved_move_end(const struct CPU *, bool success, int error);

/* Hosted entry points to exactly the vendored wrapper bodies. */
enum ClassicFileCall {
    ClassicCreateFile, ClassicWriteFile, ClassicWriteFileEx, ClassicFlushFileBuffers,
    ClassicCloseHandle, ClassicMoveFile, ClassicDeleteFile,
};
void classic_runtime_file_call(struct CPU *, enum ClassicFileCall);

#endif
