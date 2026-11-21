/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "fresh_load.h"

typedef struct {
    const struct CPU *owner;
    uint64_t generation;
    uint32_t stream, handle;
    int directory, fd;
    bool opening;
    size_t leaf_offset;
    char directory_path[CLASSIC_FRESH_PATH_BYTES];
    char path[CLASSIC_FRESH_PATH_BYTES];
    struct stat selected;
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

/* Staged native wrapper calls. The GFile carries this token independently
 * of the existing write observer. */
uint64_t classic_runtime_loaded_opened(const struct CPU *, const char *mapped, int fd, bool shared,
    uint32_t access, uint32_t share, uint32_t disposition);
void classic_runtime_loaded_bound(const struct CPU *, uint64_t generation, uint32_t handle);
void classic_runtime_loaded_closed(uint64_t generation);
