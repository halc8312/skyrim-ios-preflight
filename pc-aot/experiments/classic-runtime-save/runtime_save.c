/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime_save.h"
#include "save_paths.h"
#include "x86.h"

#include <stdatomic.h>

enum { OPEN_RETURN = 0xf6bf24, CLOSE_RETURN = 0xf67fc8, MOVE_RETURN = 0xf53f17 };
static _Thread_local ClassicRuntimeSave *local_save;
/* Zero is idle. The low bit marks an unusable generation. Value 1 reserves the
 * observer while its one owner prepares the next even token. */
static _Atomic uint64_t active_token;
static uint64_t previous_token;

static void poison(uint64_t token) {
    if (!token) return;
    atomic_compare_exchange_strong_explicit(&active_token, &token, token | 1u,
                                            memory_order_acq_rel, memory_order_relaxed);
}

static ClassicRuntimeSave *owned(const struct CPU *cpu) {
    ClassicRuntimeSave *save = local_save;
    if (!save || save->owner != cpu) return NULL;
    return atomic_load_explicit(&active_token, memory_order_acquire) == save->token ? save : NULL;
}

static bool matches_directory(int fd, const char *name) {
    struct stat open, path;
    return !fstat(fd, &open) && !lstat(name, &path) && S_ISDIR(open.st_mode) && S_ISDIR(path.st_mode) &&
           skse_save_same_file(&open, &path);
}

bool classic_runtime_save_begin(ClassicRuntimeSave *save, const struct CPU *cpu, int dir,
                               const char *directory, const char *leaf) {
    if (!save || !cpu || !directory || local_save) return false;
    const size_t length = strnlen(directory, CLASSIC_RUNTIME_PATH_BYTES);
    if (!length || directory[0] != '/' || length >= CLASSIC_RUNTIME_PATH_BYTES || directory[length - 1] == '/')
        return false;
    if (!matches_directory(dir, directory)) return false;
    uint64_t idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&active_token, &idle, 1, memory_order_acq_rel, memory_order_relaxed))
        return false;
    *save = (ClassicRuntimeSave){.owner = cpu};
    if (previous_token >= UINT64_MAX - 1 || !classic_save_start(&save->file, dir, leaf)) {
        atomic_store_explicit(&active_token, 0, memory_order_release);
        return false;
    }
    memcpy(save->directory, directory, length + 1);
    previous_token += 2;
    save->token = previous_token;
    local_save = save;
    atomic_store_explicit(&active_token, save->token, memory_order_release);
    return true;
}

void classic_runtime_save_end(ClassicRuntimeSave *save) {
    if (!save || local_save != save) return;
    local_save = NULL;
    classic_save_dispose(&save->file);
    atomic_store_explicit(&active_token, 0, memory_order_release);
}

int classic_runtime_save_reader(const ClassicRuntimeSave *save) {
    return save && save == local_save && owned(save->owner) ? classic_save_published_fd(&save->file) : -1;
}

static const char *local_leaf(const ClassicRuntimeSave *save, const char *mapped) {
    const size_t length = strlen(save->directory);
    return !strncmp(mapped, save->directory, length) && mapped[length] == '/' ? mapped + length + 1 : "";
}

void classic_runtime_save_open_scope(const struct CPU *cpu, bool opening) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save) return;
    if (save->opening == opening || save->final_scope) poison(save->token);
    else save->opening = opening;
}

uint64_t classic_runtime_saved_opened(const struct CPU *cpu, const char *mapped, int fd, bool shared) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save || !save->opening || M32(cpu->r[4]) != OPEN_RETURN) return 0;
    if (shared || !matches_directory(save->file.directory, save->directory)) {
        poison(save->token);
        return 0;
    }
    classic_save_opened(&save->file, local_leaf(save, mapped), fd, save->token);
    return save->file.outcome == CLASSIC_SAVE_PENDING ? save->token : 0;
}

void classic_runtime_saved_bound(const struct CPU *cpu, uint64_t token, uint32_t handle) {
    ClassicRuntimeSave *save = owned(cpu);
    if (save && save->token == token && !save->guest_handle) save->guest_handle = handle;
    else poison(token);
}

bool classic_runtime_saved_io(const struct CPU *cpu, uint64_t token) {
    if (!token) return false;
    ClassicRuntimeSave *save = owned(cpu);
    if (save && save->token == token && !save->os_close_seen && !save->final_scope) return true;
    poison(token);
    return false;
}

void classic_runtime_saved_written(const struct CPU *cpu, uint64_t token, int fd, size_t requested, ssize_t actual) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save || save->token != token) return;
    ++save->writes;
    save->last_requested = requested;
    save->last_written = actual;
    classic_save_written(&save->file, fd, token, requested, actual);
}

void classic_runtime_save_flush_result(const struct CPU *cpu, bool success) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save) return;
    if (!save->guest_handle || save->opening || save->os_close_seen) {
        poison(save->token);
        return;
    }
    save->flush_seen = true;
    classic_save_flushed(&save->file, save->file.writer, save->token, success);
}

void classic_runtime_saved_flush_stub(const struct CPU *cpu) {
    ClassicRuntimeSave *save = owned(cpu);
    if (save && save->guest_handle && save->guest_handle == M32(cpu->r[4] + 4)) ++save->durability_stub_calls;
}

void classic_runtime_saved_closed(const struct CPU *cpu, uint64_t token, uint32_t handle, int fd,
                                  const struct stat *before, int result, int error) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save || save->token != token) return;
    if (M32(cpu->r[4]) != CLOSE_RETURN || save->guest_handle != handle || save->file.writer != fd ||
        !save->flush_seen || save->os_close_seen) {
        poison(token);
        return;
    }
    save->os_close_seen = true;
    save->close_result = result;
    save->close_error = error;
    if (before) {
        save->before_close = *before;
        save->have_before_close = true;
    }
}

void classic_runtime_save_crt_close_result(const struct CPU *cpu, bool success) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save) return;
    if (!save->os_close_seen || save->crt_close_seen) {
        poison(save->token);
        return;
    }
    save->crt_close_seen = true;
    const struct stat *before = save->have_before_close ? &save->before_close : NULL;
    classic_save_closed(&save->file, save->file.writer, save->token, before, save->close_result, success);
}

void classic_runtime_save_final_scope(const struct CPU *cpu) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save) return;
    if (save->opening || save->final_scope || save->move_seen || !save->crt_close_seen) poison(save->token);
    else save->final_scope = true;
}

bool classic_runtime_saved_move_begin(const struct CPU *cpu, const char *from, const char *to) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save || !save->final_scope) return false;
    if (save->move_seen || M32(cpu->r[4]) != MOVE_RETURN || !matches_directory(save->file.directory, save->directory)) {
        poison(save->token);
        return false;
    }
    save->move_seen = true;
    classic_save_move_enter(&save->file, local_leaf(save, from), local_leaf(save, to));
    return true;
}

void classic_runtime_saved_move_end(const struct CPU *cpu, bool success, int error) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save) return;
    save->renamed = success;
    save->move_error = error;
}

void classic_runtime_save_crt_move_result(const struct CPU *cpu, bool success) {
    ClassicRuntimeSave *save = owned(cpu);
    if (!save) return;
    if (!save->final_scope || !save->move_seen) {
        poison(save->token);
        return;
    }
    save->final_scope = false;
    classic_save_move_leave(&save->file, save->renamed, success);
}
