/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "native_load.h"
#include "lift_prelude.h"
#include "rt.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static _Thread_local ClassicNativeLoad *armed;

static ClassicNativeLoad *current(const struct CPU *cpu) {
    return armed && armed->cpu == cpu && cpu == rt_current_cpu() ? armed : NULL;
}

enum {
    SELECTED_SLOT_RETURN = 0x68227a,
    SLOT_READ_FRAME = 0x354,
    VM_SINGLETON = 0x12e568c,
    VM_LOAD_RETURN = 0x8d6ca3,
    VM_WRAPPER_FRAME = 0x14,
    VM_WRAPPER_RETURN = 0x67dce8,
    VM_STREAM_ADAPTER = 0x34,
    VM_SECTION_FRAME = 0x60,
    VM_LOAD_POP = 12,
    VM_REVERT_RETURN = 0x67d0db,
    VM_REVERT_CASE_FRAME = 4,
    VM_REVERT_CASE_RETURN = 0x67d35c,
    VM_REVERT_RANGE_FRAME = 0x34,
    VM_REVERT_RANGE_RETURN = 0x67dc29,
    VM_REVERT_ALL_FRAME = 0x60,
    VM_REVERT_ALL_RETURN = 0x67b93b,
    VM_REVERT_SLOT_FRAME = 0x3b0,
    VM_REVERT_POP = 4,
};

static bool stack_range(const struct CPU *cpu, uint32_t first, uint32_t last) {
    const uintptr_t slot = (uintptr_t)cpu->thread;
    if (!MEM || slot >= GUEST_STACK_SLOTS || cpu->fs_base != GUEST_TEB_BASE + slot * 0x2000u) return false;
    const uint32_t lo = GUEST_STACK_BASE + (uint32_t)slot * GUEST_STACK_SIZE;
    const uint32_t hi = lo + GUEST_STACK_SIZE - 64u;
    return ((first | last) & 3u) == 0 && first >= lo && last <= hi - 4 && first <= last &&
           M32(cpu->fs_base + 4) == hi && M32(cpu->fs_base + 8) == lo;
}

static bool selected_slot(const ClassicNativeLoad *load, const struct CPU *cpu,
                          uint32_t entry, uint32_t required) {
    const uint32_t stack = load->slot_stack;
    return load->event.ticket && load->runtime_open && !load->withdrawn &&
           stack > required && entry <= stack - required && stack_range(cpu, entry, stack + 4) &&
           M32(stack) == SELECTED_SLOT_RETURN && M32(stack + 4) == load->runtime.stream;
}

static bool slot_current(const ClassicNativeLoad *load, SkyrimLifecycleCall call) {
    return call.site == SkyrimLoadSlot && call.ticket == load->event.ticket &&
           call.generation == load->lifecycle.generation;
}

bool classic_native_load_arm(ClassicNativeLoad *load, const struct CPU *cpu, SkseEventBridge *bridge,
                             int directory, const char *directory_path, int scratch) {
    if (!load || armed || !cpu || cpu != rt_current_cpu() || !bridge ||
        directory < 0 || !directory_path || scratch < 0) return false;
    *load = (ClassicNativeLoad){.cpu = cpu, .bridge = bridge, .directory_path = directory_path,
                              .directory = directory, .scratch = scratch, .consumed = -1};
    armed = load;
    return true;
}

bool classic_fresh_enabled(void) {
    ClassicNativeLoad *load = current(rt_current_cpu());
    return load && load->lifecycle_open;
}

bool classic_fresh_resolve_path(const struct CPU *cpu, uint32_t path,
                                char canonical[CLASSIC_FRESH_PATH_BYTES]) {
    ClassicNativeLoad *load = current(cpu);
    return load && classic_fresh_resolve_selected_guest_path(cpu, path, load->directory_path, canonical);
}

uint64_t classic_native_load_enter(const struct CPU *cpu) {
    ClassicNativeLoad *load = current(cpu);
    if (!load || load->reported) return 0;
    if (load->ticket) {
        /* Let the existing selected-load observer invalidate the outer
         * attempt on reentry. An inner invocation may still load normally. */
        (void)classic_fresh_enter(cpu);
        load->withdrawn = true;
        if (load->runtime_open) {
            classic_runtime_load_end(&load->runtime);
            load->runtime_open = false;
        }
        return 0;
    }
    if (!skyrim_lifecycle_scope_open(&load->lifecycle)) return 0;
    load->lifecycle_open = true;
    load->ticket = classic_fresh_enter(cpu);
    if (!load->ticket) {
        skyrim_lifecycle_scope_close(&load->lifecycle);
        load->lifecycle_open = false;
    }
    return load->ticket;
}

void classic_native_load_open_enter(uint64_t ticket, const struct CPU *cpu, uint32_t stream, uint32_t path) {
    ClassicNativeLoad *load = current(cpu);
    if (!load || !ticket || load->ticket != ticket || load->runtime_open || load->withdrawn) return;
    const int saved = errno;
    classic_fresh_open_enter(ticket, cpu, stream, path);
    load->runtime_open = classic_runtime_load_begin(&load->runtime, cpu, stream, path,
                                                    load->directory, load->directory_path);
    if (load->runtime_open) (void)classic_runtime_load_enable_capture(&load->runtime, load->bridge, load->scratch);
    errno = saved;
}

void classic_native_load_open_leave(uint64_t ticket, const struct CPU *cpu, bool success) {
    ClassicNativeLoad *load = current(cpu);
    if (!load || !ticket || load->ticket != ticket) return;
    const int saved = errno;
    if (load->runtime_open) {
        ClassicRuntimeLoad *runtime = &load->runtime;
        classic_runtime_load_open_leave(runtime, success);
        if (success && runtime->pending_input && classic_runtime_load_bind_crt(cpu, runtime->stream))
            classic_fresh_observe_open(cpu, runtime->stream, runtime->path, runtime->fd, runtime->generation);
    }
    classic_fresh_open_leave(ticket, cpu, success);
    errno = saved;
}

void classic_native_load_slot_bind(uint64_t ticket, const struct CPU *cpu, uint32_t stream) {
    ClassicNativeLoad *load = current(cpu);
    if (!load || !ticket || load->ticket != ticket || !load->runtime_open || load->event.ticket) return;
    ClassicRuntimeLoad *runtime = &load->runtime;
    const uint32_t stack = cpu->r[4];
    if (!runtime->pending_input || runtime->stream != stream ||
        !stack_range(cpu, stack, stack + 4) || M32(stack) != SELECTED_SLOT_RETURN || M32(stack + 4) != stream ||
        !classic_runtime_load_generation(cpu, stream, runtime->handle)) return;
    if (skse_bridge_bind_load_slot(runtime->pending_input, &load->event) == SKSE_BRIDGE_OK)
        load->slot_stack = stack;
}

void classic_native_load_read_observed(const struct CPU *cpu, uint32_t stream,
                                      uint32_t entry_stack, uint32_t bytes) {
    ClassicNativeLoad *load = current(cpu);
    if (!load || !load->event.ticket) return;
    const int saved = errno;
    const SkyrimLifecycleCall slot = skyrim_lifecycle_scope_current();
    if (slot_current(load, slot)) {
        const uint64_t generation = classic_runtime_load_slot_read_end(cpu, stream, entry_stack, bytes);
        if (generation) classic_fresh_observe_read(cpu, stream, load->runtime.fd, generation, bytes);
    }
    errno = saved;
}

uint32_t classic_native_load_read_begin(const struct CPU *cpu, uint32_t stream) {
    const ClassicNativeLoad *load = current(cpu);
    if (!load || !slot_current(load, skyrim_lifecycle_scope_current()) ||
        load->slot_stack < SLOT_READ_FRAME || cpu->r[4] != load->slot_stack - SLOT_READ_FRAME) return 0;
    const int saved = errno;
    const uint32_t entry = classic_runtime_load_slot_read_begin(cpu, stream);
    errno = saved;
    return entry;
}

SkyrimLifecycleCall classic_native_load_vm_begin(const struct CPU *cpu, uint32_t entry) {
    const ClassicNativeLoad *load = current(cpu);
    if (!load || cpu->r[4] != entry || !slot_current(load, skyrim_lifecycle_scope_current()) ||
        !selected_slot(load, cpu, entry, VM_SECTION_FRAME + 8) ||
        !cpu->r[1] || cpu->r[1] != TL32(VM_SINGLETON)) return (SkyrimLifecycleCall){0};

    if (M32(entry) == VM_REVERT_RETURN && load->slot_stack - entry == VM_REVERT_SLOT_FRAME &&
        M32(entry + VM_REVERT_CASE_FRAME) == VM_REVERT_CASE_RETURN &&
        M32(entry + VM_REVERT_RANGE_FRAME) == VM_REVERT_RANGE_RETURN &&
        M32(entry + VM_REVERT_ALL_FRAME) == VM_REVERT_ALL_RETURN)
        return skyrim_lifecycle_begin(SkyrimRevertForLoad);

    const uint32_t section = entry + VM_SECTION_FRAME;
    const uint32_t caller = M32(section);
    if (M32(entry) == VM_LOAD_RETURN && M32(entry + 4) == entry + VM_STREAM_ADAPTER &&
        M32(entry + 8) == entry + 0x13 && M32(entry + VM_WRAPPER_FRAME) == VM_WRAPPER_RETURN &&
        M32(entry + VM_WRAPPER_FRAME + 4) == M32(entry + 4) &&
        (caller == 0x67b955 || caller == 0x67bca5 || caller == 0x67bf50) &&
        M32(section + 4) == load->runtime.stream)
        return skyrim_lifecycle_begin(SkyrimLoadVM);

    return (SkyrimLifecycleCall){0};
}

void classic_native_load_vm_leave(const struct CPU *cpu, SkyrimLifecycleCall call, uint32_t entry) {
    if (!call.ticket) return;
    const ClassicNativeLoad *load = current(cpu);
    if (!load || call.generation != load->lifecycle.generation || call.parent != load->event.ticket) return;
    if (call.site == SkyrimLoadVM && cpu->r[4] == entry + VM_LOAD_POP)
        skyrim_lifecycle_end(call, (uint8_t)cpu->r[0] ? SkyrimReturnedTrue : SkyrimReturnedFalse);
    if (call.site == SkyrimRevertForLoad && cpu->r[4] == entry + VM_REVERT_POP)
        skyrim_lifecycle_end(call, SkyrimNoResult);
}

void classic_fresh_report(ClassicFreshReport report) {
    ClassicNativeLoad *load = current(rt_current_cpu());
    if (!load || report.ticket != load->ticket || load->reported) return;
    const int saved = errno;
    load->reported = true;
    load->result = report.result;
    if (report.result == CLASSIC_FRESH_READY && load->event.ticket)
        load->consumed = fcntl(report.ess_fd, F_DUPFD_CLOEXEC, 0);
    errno = saved;
}

void classic_native_load_abort(ClassicNativeLoad *load) {
    if (!load || load != current(rt_current_cpu())) return;
    const int saved = errno;
    classic_fresh_cancel(load->ticket, load->cpu);
    if (load->consumed >= 0) close(load->consumed);
    if (load->runtime_open) classic_runtime_load_end(&load->runtime);
    if (load->lifecycle_open) (void)skyrim_lifecycle_scope_close(&load->lifecycle);
    armed = NULL;
    *load = (ClassicNativeLoad){0};
    errno = saved;
}

SkseBridgeResult classic_native_load_finish(ClassicNativeLoad *load, void **image, size_t *length,
                                           SksePairResult *detail) {
    if (image) *image = NULL;
    if (length) *length = 0;
    if (detail) *detail = SKSE_PAIR_OK;
    if (!image || !length) return SKSE_BRIDGE_ARGUMENT;
    if (!load || load != current(rt_current_cpu()) || !load->reported) return SKSE_BRIDGE_STATE;
    SkseBridgeResult result = SKSE_BRIDGE_NEEDS_BASE;
    if (load->runtime_open && load->runtime.pending_input && load->event.ticket) {
        uint64_t dropped = 0;
        SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
        SkseEventTrace trace = {.rows = rows};
        memcpy(trace.launch, load->event.scope.launch, sizeof trace.launch);
        trace.count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &dropped);
        trace.dropped = dropped;
        const SkseBaseEvidence proof = {load->event,
            load->consumed >= 0 ? SKSE_BASE_LOADED : SKSE_BASE_FAILED, load->consumed};
        result = skse_bridge_finish_load_input(&load->runtime.pending_input, &trace, &proof, image, length, detail);
    }
    classic_native_load_abort(load);
    return result;
}
