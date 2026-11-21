/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "runtime_load.h"

typedef struct {
    const struct CPU *cpu;
    SkseEventBridge *bridge;
    const char *directory_path;
    int directory, scratch, consumed;
    uint64_t ticket;
    uint32_t slot_stack;
    bool lifecycle_open, runtime_open, reported, withdrawn;
    ClassicRuntimeLoad runtime;
    SkyrimLifecycleScope lifecycle;
    SkseGameEvent event;
    ClassicFreshResult result;
} ClassicNativeLoad;

/* The bridge, directory descriptors and mapped path remain live on the
 * owner thread through finish or abort. Neither operation closes GFile. */
bool classic_native_load_arm(ClassicNativeLoad *, const struct CPU *, SkseEventBridge *,
                             int directory, const char *directory_path, int scratch);
SkseBridgeResult classic_native_load_finish(ClassicNativeLoad *, void **image, size_t *length,
                                           SksePairResult *detail);
void classic_native_load_abort(ClassicNativeLoad *);

/* Hooks for the pinned outer load and its one logical slot read. */
uint64_t classic_native_load_enter(const struct CPU *);
void classic_native_load_open_enter(uint64_t, const struct CPU *, uint32_t stream, uint32_t path);
void classic_native_load_open_leave(uint64_t, const struct CPU *, bool success);
void classic_native_load_slot_bind(uint64_t, const struct CPU *, uint32_t stream);
uint32_t classic_native_load_read_begin(const struct CPU *, uint32_t stream);
void classic_native_load_read_observed(const struct CPU *, uint32_t stream,
                                      uint32_t entry_stack, uint32_t bytes);

/* These return an empty token outside the reviewed selected-load frames. */
SkyrimLifecycleCall classic_native_load_vm_begin(const struct CPU *, uint32_t entry_stack);
void classic_native_load_vm_leave(const struct CPU *, SkyrimLifecycleCall, uint32_t entry_stack);
