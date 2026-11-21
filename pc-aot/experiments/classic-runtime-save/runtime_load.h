/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "fresh_load.h"

#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
#include "event_bridge.h"
#endif

typedef struct {
    uint32_t bsfile, callback, file, file_slot, fileno, descriptor, handle;
} ClassicRuntimeCrtFile;

typedef struct {
    const struct CPU *owner;
    uint64_t generation;
    uint32_t stream, handle;
    int directory, fd;
    bool opening, crt_attempted;
    ClassicRuntimeCrtFile crt;
    size_t leaf_offset;
    char directory_path[CLASSIC_FRESH_PATH_BYTES];
    char path[CLASSIC_FRESH_PATH_BYTES];
    struct stat selected;
#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
    SkseEventBridge *bridge;
    SkseLoadInput *pending_input;
    int scratch, source_fd;
    struct stat snapshot;
#endif
} ClassicRuntimeLoad;

/* The caller keeps the state alive on the current guest owner thread.
 * The directory descriptor is duplicated for the observation lifetime. */
bool classic_runtime_load_begin(ClassicRuntimeLoad *, const struct CPU *, uint32_t stream,
    uint32_t guest_path, int directory, const char *mapped_directory);
void classic_runtime_load_open_leave(ClassicRuntimeLoad *, bool succeeded);
void classic_runtime_load_end(ClassicRuntimeLoad *);

/* Return a live generation for this native handle and receiver. The caller
 * must still establish that the CRT uses this handle for its logical read. */
uint64_t classic_runtime_load_generation(const struct CPU *, uint32_t stream, uint32_t handle);

/* Opt-in hosted observation immediately after the verified first open. */
bool classic_runtime_load_bind_crt(const struct CPU *, uint32_t stream);

/* The original logical read still owns its call and return. The stack token
 * brackets that exact slot read, and a zero token carries no evidence. */
uint32_t classic_runtime_load_slot_read_begin(const struct CPU *, uint32_t stream);
uint64_t classic_runtime_load_slot_read_end(const struct CPU *, uint32_t stream,
                                           uint32_t entry_stack, uint32_t bytes);

/* Staged native wrapper calls. The GFile carries this token independently
 * of the existing write observer. */
uint64_t classic_runtime_loaded_opened(const struct CPU *, const char *mapped, int fd, bool shared,
    uint32_t access, uint32_t share, uint32_t disposition);
void classic_runtime_loaded_bound(const struct CPU *, uint64_t generation, uint32_t handle);
void classic_runtime_loaded_closed(uint64_t generation);

#if defined(SKYRIM_EXPERIMENT_CLONE_LOAD)
/* Bridge and scratch directory are borrowed for the owner load scope. This
 * hosted stage drops any unused pending input when the scope ends. */
bool classic_runtime_load_enable_capture(ClassicRuntimeLoad *, SkseEventBridge *, int scratch);

/* Called between the checked original open and native handle publication.
 * On success the wrapper owns both returned and original descriptors. */
int classic_runtime_loaded_prepare_clone(const struct CPU *, uint64_t generation,
                                         int source, struct stat *named_metadata);

enum ClassicLoadFileCall {
    ClassicLoadRead, ClassicLoadSeek, ClassicLoadSize, ClassicLoadInformation,
};
void classic_runtime_load_file_call(struct CPU *, enum ClassicLoadFileCall);
#endif
