/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime_load.h"
#include "lift_prelude.h"
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

#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
static bool clone_version(int fd, struct stat *out) {
    const int flags = fcntl(fd, F_GETFL), descriptors = fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && descriptors >= 0 &&
           (descriptors & FD_CLOEXEC) && !fstat(fd, out) &&
           S_ISREG(out->st_mode) && out->st_nlink == 0;
}

bool classic_runtime_load_enable_capture(ClassicRuntimeLoad *load, SkseEventBridge *bridge, int scratch) {
    if (!load || load != owned(rt_current_cpu()) || !load->opening || load->fd >= 0 ||
        !bridge || load->bridge || scratch < 0) return false;
    load->bridge = bridge;
    load->scratch = scratch;
    return true;
}

int classic_runtime_loaded_prepare_clone(const struct CPU *cpu, uint64_t generation,
                                         int source, struct stat *named_metadata) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || load->generation != generation || !load->opening || load->handle ||
        load->fd != source || !load->bridge || load->pending_input || !named_metadata) return -1;
    const int saved_errno = errno;
    int fd = -1;
    SkseLoadInput *input = NULL;
    struct stat snapshot;
    if (skse_bridge_capture_named_load(load->bridge, skyrim_lifecycle_scope_current, load->directory,
                                     load->path + load->leaf_offset, source, load->scratch,
                                     &input, &fd, NULL) != SKSE_BRIDGE_OK ||
        !clone_version(fd, &snapshot) || skse_save_same_file(&snapshot, &load->selected) ||
        snapshot.st_size != load->selected.st_size || lseek(fd, 0, SEEK_CUR) != 0 ||
        !same_selection(load)) {
        if (fd >= 0) close(fd);
        skse_bridge_drop_load_input(input);
        errno = saved_errno;
        return -1;
    }
    load->snapshot = snapshot;
    load->source_fd = source;
    load->fd = fd;
    load->pending_input = input;
    *named_metadata = load->selected;
    errno = saved_errno;
    return fd;
}
#endif

static bool same_input(const ClassicRuntimeLoad *load) {
#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
    if (load->source_fd >= 0) {
        struct stat snapshot;
        return clone_version(load->fd, &snapshot) && skse_save_same_version(&snapshot, &load->snapshot);
    }
#endif
    return same_selection(load);
}


bool classic_runtime_load_begin(ClassicRuntimeLoad *load, const struct CPU *cpu, uint32_t stream,
    uint32_t guest_path, int directory, const char *mapped_directory) {
    if (!load || local_load || !cpu || cpu != rt_current_cpu() || !stream ||
        stream != cpu->r[1] || directory < 0) return false;
    const int previous_errno = errno;
    ClassicRuntimeLoad next = {.owner = cpu, .stream = stream, .fd = -1, .opening = true};
#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
    next.source_fd = -1;
#endif
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
        handle && handle != UINT32_MAX && same_input(load)) load->handle = handle;
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
    if (!same_input(load)) invalidate(load->generation);
    errno = previous_errno;
    return owned(cpu) ? load->generation : 0;
}

/* Fixed Classic 1.9.32 CRT and f_67B720 call layouts. These reads only
 * observe the owner's live stream and the runtime's reserved guest map. */
enum {
    BSFILE_LINK = 0x104, BSFILE_SIZE = 0x15c, BSFILE_VTABLE = 0x110dba8,
    READ_CALLBACK = 0xa57260, READ_CALLBACK_SWAPPED = 0xa572a0, READ_VIRTUAL = 0xa57e70,
    CRT_IOS = 0x1bc046c, CRT_IOS_COUNT = 0x1bc1480, CRT_IOS_MAX = 512,
    CRT_STATIC_FILE = 0x12de588, CRT_STATIC_COUNT = 20, CRT_FILE_BYTES = 0x20,
    CRT_DESCRIPTOR_COUNT = 0x1bc0320, CRT_DESCRIPTOR_PAGES = 0x1bc0340,
    CRT_PAGE_COUNT = 64, CRT_PAGE_ENTRIES = 32, CRT_ENTRY_BYTES = 0x40,
    SLOT_READ_RETURN = 0x67b875, SLOT_PARENT_RETURN = 0x68227a,
    SLOT_PARENT_OFFSET = 0x354, SLOT_DESTINATION_OFFSET = 0xa4, SLOT_RETURN_POP = 12,
};

static bool guest_object(uint32_t address, uint32_t size, uint32_t end) {
    return !(address & 3u) && address >= GUEST_HEAP_BASE && address <= end - size;
}

static uint32_t crt_file_slot(uint32_t file) {
    const uint32_t table = TL32(CRT_IOS), count = TL32(CRT_IOS_COUNT);
    if (count < CRT_STATIC_COUNT || count > CRT_IOS_MAX ||
        !guest_object(table, count * 4u, GUEST_HEAP_END)) return 0;

    const uint32_t static_end = CRT_STATIC_FILE + CRT_STATIC_COUNT * CRT_FILE_BYTES;
    if (file >= CRT_STATIC_FILE && file < static_end) {
        const uint32_t index = (file - CRT_STATIC_FILE) / CRT_FILE_BYTES;
        const uint32_t slot = table + index * 4u;
        return index >= 3 && file == CRT_STATIC_FILE + index * CRT_FILE_BYTES &&
               TL32(slot) == file ? slot : 0;
    }
    if (!guest_object(file, CRT_FILE_BYTES, GUEST_HEAP_END)) return 0;
    for (uint32_t index = CRT_STATIC_COUNT; index < count; ++index) {
        const uint32_t slot = table + index * 4u;
        if (TL32(slot) == file) return slot;
    }
    return 0;
}

static bool read_crt_file(uint32_t stream, ClassicRuntimeCrtFile *link) {
    if (!MEM || !guest_object(stream, BSFILE_LINK + 4, GUEST_VA_END)) return false;
    const uint32_t bsfile = TL32(stream + BSFILE_LINK);
    if (!guest_object(bsfile, BSFILE_SIZE, GUEST_VA_END) || TL32(bsfile) != BSFILE_VTABLE ||
        TL32(BSFILE_VTABLE + 0x44) != READ_VIRTUAL || !TL8(bsfile + 0x2c)) return false;
    const uint32_t callback = TL32(bsfile + 8);
    if (callback != READ_CALLBACK && callback != READ_CALLBACK_SWAPPED) return false;

    const uint32_t file = TL32(bsfile + 0x24);
    const uint32_t slot = crt_file_slot(file);
    /* A successful rb stream has the read bit and neither write nor update. */
    if (!slot || (TL32(file + 0xc) & 0x83u) != 1u) return false;
    const uint32_t fileno = TL32(file + 0x10), count = TL32(CRT_DESCRIPTOR_COUNT);
    if (!count || count > CRT_PAGE_COUNT * CRT_PAGE_ENTRIES || count % CRT_PAGE_ENTRIES ||
        fileno < 3 || fileno >= count) return false;
    const uint32_t page = TL32(CRT_DESCRIPTOR_PAGES + fileno / CRT_PAGE_ENTRIES * 4u);
    if (!guest_object(page, CRT_PAGE_ENTRIES * CRT_ENTRY_BYTES, GUEST_HEAP_END)) return false;
    const uint32_t descriptor = page + fileno % CRT_PAGE_ENTRIES * CRT_ENTRY_BYTES;
    if (!(TL8(descriptor + 4) & 1u)) return false;
    *link = (ClassicRuntimeCrtFile){bsfile, callback, file, slot, fileno, descriptor, TL32(descriptor)};
    return link->handle && link->handle != UINT32_MAX;
}

bool classic_runtime_load_bind_crt(const struct CPU *cpu, uint32_t stream) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || load->opening || load->crt_attempted || stream != load->stream) return false;
    load->crt_attempted = true;
    ClassicRuntimeCrtFile link;
    if (!read_crt_file(stream, &link) || !classic_runtime_load_generation(cpu, stream, link.handle)) return false;
    load->crt = link;
    return true;
}

static bool same_crt_file(const CPU *cpu, uint32_t stream) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || load->opening || !load->crt.file || stream != load->stream) return false;
    ClassicRuntimeCrtFile current;
    const ClassicRuntimeCrtFile *open = &load->crt;
    if (read_crt_file(stream, &current) && current.bsfile == open->bsfile &&
        current.callback == open->callback && current.file == open->file && current.file_slot == open->file_slot &&
        current.fileno == open->fileno && current.descriptor == open->descriptor && current.handle == open->handle)
        return true;
    invalidate(load->generation);
    return false;
}

static bool slot_read_frame(const CPU *cpu, uint32_t stream, uint32_t entry) {
    const uintptr_t slot = (uintptr_t)cpu->thread;
    if (!MEM || slot >= GUEST_STACK_SLOTS || cpu->fs_base != GUEST_TEB_BASE + slot * 0x2000u) return false;
    const uint32_t lo = GUEST_STACK_BASE + (uint32_t)slot * GUEST_STACK_SIZE;
    const uint32_t hi = lo + GUEST_STACK_SIZE - 64u;
    return !(entry & 3u) && entry >= lo && entry <= hi - SLOT_PARENT_OFFSET - 8 &&
           M32(cpu->fs_base + 4) == hi && M32(cpu->fs_base + 8) == lo &&
           M32(entry) == SLOT_READ_RETURN && M32(entry + 4) == entry + SLOT_DESTINATION_OFFSET &&
           M32(entry + 8) == CLASSIC_FRESH_SLOT_READ_BYTES &&
           M32(entry + SLOT_PARENT_OFFSET) == SLOT_PARENT_RETURN &&
           M32(entry + SLOT_PARENT_OFFSET + 4) == stream;
}

uint32_t classic_runtime_load_slot_read_begin(const struct CPU *cpu, uint32_t stream) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || stream != cpu->r[1] || stream != cpu->r[6] ||
        !slot_read_frame(cpu, stream, cpu->r[4]) || !same_crt_file(cpu, stream) ||
        !classic_runtime_load_generation(cpu, stream, load->crt.handle)) return 0;
    return cpu->r[4];
}

uint64_t classic_runtime_load_slot_read_end(const struct CPU *cpu, uint32_t stream,
                                           uint32_t entry_stack, uint32_t bytes) {
    ClassicRuntimeLoad *load = owned(cpu);
    if (!load || !entry_stack || !bytes || bytes > CLASSIC_FRESH_SLOT_READ_BYTES ||
        bytes != cpu->r[0] || stream != cpu->r[6] ||
        !slot_read_frame(cpu, stream, entry_stack) || cpu->r[4] != entry_stack + SLOT_RETURN_POP ||
        !same_crt_file(cpu, stream)) return 0;
    return classic_runtime_load_generation(cpu, stream, load->crt.handle);
}


void classic_runtime_load_end(ClassicRuntimeLoad *load) {
    if (!load || load != local_load || load->owner != rt_current_cpu()) return;
    const int previous_errno = errno;
    local_load = NULL;
#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
    skse_bridge_drop_load_input(load->pending_input);
#endif
    close(load->directory);
    *load = (ClassicRuntimeLoad){0};
    atomic_store_explicit(&active_generation, 0, memory_order_release);
    errno = previous_errno;
}
