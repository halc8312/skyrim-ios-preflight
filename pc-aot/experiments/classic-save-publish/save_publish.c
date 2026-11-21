/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "save_publish.h"
#include "save_paths.h"

static bool regular(const struct stat *info) {
    return S_ISREG(info->st_mode) && info->st_nlink == 1;
}

static bool same_closed_contents(const struct stat *closed, const struct stat *current) {
#if defined(__APPLE__)
    const struct timespec a = closed->st_mtimespec, b = current->st_mtimespec;
#else
    const struct timespec a = closed->st_mtim, b = current->st_mtim;
#endif
    /* A same-directory rename may change ctime. Contents must be unchanged. */
    return regular(current) && skse_save_same_file(closed, current) && closed->st_size == current->st_size &&
           closed->st_mode == current->st_mode && a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

static bool current_leaf(const ClassicSavePublish *save, const char *leaf) {
    struct stat held, named;
    return !fstat(save->reader, &held) && same_closed_contents(&save->closed_file, &held) &&
           !fstatat(save->directory, leaf, &named, AT_SYMLINK_NOFOLLOW) &&
           skse_save_same_version(&held, &named);
}

bool classic_save_start(ClassicSavePublish *save, int directory, const char *leaf) {
    static const char tail[] = ".tmp";
    *save = (ClassicSavePublish){.directory = directory, .writer = -1, .reader = -1};
    size_t stem = skse_save_stem(leaf);
    struct stat folder;
    if (!stem || stem + 4 + sizeof tail - 1 > NAME_MAX || fstat(directory, &folder) || !S_ISDIR(folder.st_mode)) {
        save->outcome = CLASSIC_SAVE_IDENTITY;
        return false;
    }
    size_t size = stem + 4;
    memcpy(save->final_leaf, leaf, size + 1);
    memcpy(save->temp_leaf, leaf, size);
    memcpy(save->temp_leaf + size, tail, sizeof tail);
    return true;
}

void classic_save_opened(ClassicSavePublish *save, const char *temp, int writer, uint64_t generation) {
    if (save->outcome != CLASSIC_SAVE_PENDING) return;
    const int flags = fcntl(writer, F_GETFL);
    struct stat original, retained;
    if (save->reader >= 0 || !generation || strcmp(temp, save->temp_leaf) || flags < 0 ||
        (flags & O_ACCMODE) == O_RDONLY || fstat(writer, &original) || !regular(&original)) {
        save->outcome = CLASSIC_SAVE_IDENTITY;
        return;
    }
    const int reader = openat(save->directory, temp, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (reader < 0 || fstat(reader, &retained) || !skse_save_same_version(&original, &retained)) {
        if (reader >= 0) close(reader);
        save->outcome = CLASSIC_SAVE_IDENTITY;
        return;
    }
    save->reader = reader;
    save->writer = writer;
    save->generation = generation;
}

static bool same_writer(ClassicSavePublish *save, int writer, uint64_t generation) {
    if (save->outcome != CLASSIC_SAVE_PENDING) return false;
    if (save->reader >= 0 && !save->closed && save->writer == writer && save->generation == generation) return true;
    save->outcome = CLASSIC_SAVE_IDENTITY;
    return false;
}

void classic_save_written(ClassicSavePublish *save, int writer, uint64_t generation, size_t requested, ssize_t count) {
    if (!same_writer(save, writer, generation)) return;
    if (count < 0 || (size_t)count != requested) save->outcome = CLASSIC_SAVE_WRITE;
    else if (count > 0) save->written = true;
}

void classic_save_flushed(ClassicSavePublish *save, int writer, uint64_t generation, bool success) {
    if (!same_writer(save, writer, generation)) return;
    if (!success || save->flushed) save->outcome = CLASSIC_SAVE_FLUSH;
    else save->flushed = true;
}

void classic_save_closed(ClassicSavePublish *save, int writer, uint64_t generation,
                         const struct stat *before_close, int close_result, bool crt_success) {
    if (!same_writer(save, writer, generation)) return;
    if (close_result || !crt_success || !save->written || !save->flushed) {
        save->outcome = CLASSIC_SAVE_CLOSE;
        return;
    }
    if (!before_close || fstat(save->reader, &save->closed_file) || !regular(&save->closed_file) ||
        save->closed_file.st_size <= 0 || !skse_save_same_version(before_close, &save->closed_file)) {
        save->outcome = CLASSIC_SAVE_IDENTITY;
        return;
    }
    save->closed = true;
}

void classic_save_move_enter(ClassicSavePublish *save, const char *from, const char *to) {
    if (save->outcome != CLASSIC_SAVE_PENDING) return;
    if (save->moving || !save->closed || strcmp(from, save->temp_leaf) || strcmp(to, save->final_leaf) ||
        !current_leaf(save, from)) {
        save->outcome = CLASSIC_SAVE_IDENTITY;
        return;
    }
    save->moving = true;
}

void classic_save_move_leave(ClassicSavePublish *save, bool renamed, bool crt_success) {
    if (save->outcome != CLASSIC_SAVE_PENDING) return;
    if (!save->moving || !renamed || !crt_success) save->outcome = CLASSIC_SAVE_MOVE;
    else save->outcome = current_leaf(save, save->final_leaf) ? CLASSIC_SAVE_PUBLISHED : CLASSIC_SAVE_IDENTITY;
    save->moving = false;
}

int classic_save_published_fd(const ClassicSavePublish *save) {
    return save->outcome == CLASSIC_SAVE_PUBLISHED && current_leaf(save, save->final_leaf) ? save->reader : -1;
}

void classic_save_dispose(ClassicSavePublish *save) {
    if (save->reader >= 0) close(save->reader);
    save->reader = -1;
    save->outcome = CLASSIC_SAVE_PENDING;
}
