/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime_load.h"
#include "rt.h"
#include "save_paths.h"

#include <stdatomic.h>

enum { CRT_OPEN_RETURN = 0xf6bf24, FILE_SHARE_READ = 1, OPEN_EXISTING = 3 };
static const uint32_t GENERIC_READ_ACCESS = UINT32_C(0x80000000);

static _Thread_local ClassicRuntimeLoad *local_load;
/* Zero is idle, one reserves the next even token, and the low bit marks
 * a closed or otherwise invalid generation. Cross-thread close only uses
 * the atomic token, without following the owner's state. */
static _Atomic uint64_t active_generation;
static uint64_t last_generation;

static void invalidate(uint64_t generation) {
    if (generation)
        atomic_compare_exchange_strong_explicit(&active_generation, &generation, generation | 1u,
                                                memory_order_acq_rel, memory_order_relaxed);
}

static ClassicRuntimeLoad *owned(const struct CPU *cpu) {
    ClassicRuntimeLoad *load = local_load;
    return load && cpu == load->owner && cpu == rt_current_cpu() &&
           atomic_load_explicit(&active_generation, memory_order_acquire) == load->generation ? load : NULL;
}

static bool selected_entry(const ClassicRuntimeLoad *load, struct stat *out) {
    struct stat checked_dir, named_dir;
    return !fstat(load->directory, &checked_dir) && !lstat(load->directory_path, &named_dir) &&
           S_ISDIR(checked_dir.st_mode) && S_ISDIR(named_dir.st_mode) &&
           skse_save_same_file(&checked_dir, &named_dir) &&
           !fstatat(load->directory, load->path + load->leaf_offset, out, AT_SYMLINK_NOFOLLOW) &&
           S_ISREG(out->st_mode) && out->st_size > 0;
}

static bool same_selection(const ClassicRuntimeLoad *load) {
    struct stat opened, named;
    const int flags = fcntl(load->fd, F_GETFL);
    return flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && !fstat(load->fd, &opened) &&
           skse_save_same_version(&load->selected, &opened) &&
           selected_entry(load, &named) && skse_save_same_version(&opened, &named);
}

bool classic_runtime_load_begin(ClassicRuntimeLoad *load, const struct CPU *cpu, uint32_t stream,
    uint32_t guest_path, int directory, const char *mapped_directory) {
    if (!load || local_load || !cpu || cpu != rt_current_cpu() || !stream ||
        stream != cpu->r[1] || directory < 0) return false;
    const int previous_errno = errno;
    ClassicRuntimeLoad next = {.owner = cpu, .stream = stream, .fd = -1, .opening = true};
    if (!classic_fresh_resolve_selected_guest_path(cpu, guest_path, mapped_directory, next.path)) return false;
    const size_t prefix = strlen(mapped_directory);
    next.leaf_offset = prefix + 1;
    memcpy(next.directory_path, mapped_directory, prefix + 1);

    uint64_t idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&active_generation, &idle, 1,
                                                 memory_order_acq_rel, memory_order_relaxed)) return false;
    next.directory = fcntl(directory, F_DUPFD_CLOEXEC, 0);
    if (next.directory < 0 || last_generation >= UINT64_MAX - 1 || !selected_entry(&next, &next.selected)) {
        if (next.directory >= 0) close(next.directory);
        atomic_store_explicit(&active_generation, 0, memory_order_release);
        errno = previous_errno;
        return false;
    }
    last_generation += 2;
    next.generation = last_generation;
    *load = next;
    local_load = load;
    atomic_store_explicit(&active_generation, load->generation, memory_order_release);
    errno = previous_errno;
    return true;
}

uint64_t classic_runtime_loaded_opened(const struct CPU *cpu, const char *mapped, int fd, bool shared,
    uint32_t access, uint32_t share, uint32_t disposition) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || !load->opening || M32(cpu->r[4]) != CRT_OPEN_RETURN) return 0;
    if (load->fd >= 0 || shared || access != GENERIC_READ_ACCESS || share != FILE_SHARE_READ ||
        disposition != OPEN_EXISTING || strcmp(mapped, load->path)) {
        invalidate(load->generation);
        return 0;
    }
    load->fd = fd;
    if (!same_selection(load)) {
        invalidate(load->generation);
        return 0;
    }
    return load->generation;
}

void classic_runtime_loaded_bound(const struct CPU *cpu, uint64_t generation, uint32_t handle) {
    ClassicRuntimeLoad *load = owned(cpu);
    const int previous_errno = errno;
    if (load && load->generation == generation && load->opening && !load->handle &&
        handle && handle != UINT32_MAX && same_selection(load)) load->handle = handle;
    else invalidate(generation);
    errno = previous_errno;
}

void classic_runtime_loaded_closed(uint64_t generation) { invalidate(generation); }

void classic_runtime_load_open_leave(ClassicRuntimeLoad *load, bool succeeded) {
    if (!load || load != local_load || load->owner != rt_current_cpu()) return;
    if (!succeeded || !load->opening || !load->handle) invalidate(load->generation);
    load->opening = false;
}

uint64_t classic_runtime_load_generation(const struct CPU *cpu, uint32_t stream, uint32_t handle) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || load->opening || load->stream != stream || !handle || handle != load->handle) return 0;
    const int previous_errno = errno;
    if (!same_selection(load)) invalidate(load->generation);
    errno = previous_errno;
    return owned(cpu) ? load->generation : 0;
}

void classic_runtime_load_end(ClassicRuntimeLoad *load) {
    if (!load || load != local_load || load->owner != rt_current_cpu()) return;
    const int previous_errno = errno;
    local_load = NULL;
    close(load->directory);
    *load = (ClassicRuntimeLoad){0};
    atomic_store_explicit(&active_generation, 0, memory_order_release);
    errno = previous_errno;
}
