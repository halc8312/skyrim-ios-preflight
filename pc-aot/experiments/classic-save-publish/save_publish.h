/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_SAVE_PUBLISH_H
#define CLASSIC_SAVE_PUBLISH_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef enum {
    CLASSIC_SAVE_PENDING, CLASSIC_SAVE_PUBLISHED, CLASSIC_SAVE_IDENTITY,
    CLASSIC_SAVE_WRITE, CLASSIC_SAVE_FLUSH, CLASSIC_SAVE_CLOSE, CLASSIC_SAVE_MOVE,
} ClassicSaveOutcome;

typedef struct {
    int directory, writer, reader;
    uint64_t generation;
    char final_leaf[NAME_MAX + 1], temp_leaf[NAME_MAX + 1];
    struct stat closed_file;
    ClassicSaveOutcome outcome;
    bool written, flushed, closed, moving;
} ClassicSavePublish;

/* The hosted backend owns the directory, writer and per-open token. The
 * witness owns one reader for the exact temp inode and never changes a file. */
bool classic_save_start(ClassicSavePublish *, int directory, const char *final_leaf);
void classic_save_opened(ClassicSavePublish *, const char *temp_leaf, int writer, uint64_t generation);
void classic_save_written(ClassicSavePublish *, int writer, uint64_t generation, size_t requested, ssize_t written);
void classic_save_flushed(ClassicSavePublish *, int writer, uint64_t generation, bool success);
void classic_save_closed(ClassicSavePublish *, int writer, uint64_t generation,
                         const struct stat *before_close, int close_result, bool crt_success);

/* Called only around the reviewed final MoveFile. A borrow lasts until
 * disposal. The caller must capture an immutable view before deferring work. */
void classic_save_move_enter(ClassicSavePublish *, const char *from, const char *to);
void classic_save_move_leave(ClassicSavePublish *, bool renamed, bool crt_success);
int classic_save_published_fd(const ClassicSavePublish *);
void classic_save_dispose(ClassicSavePublish *);

#endif
