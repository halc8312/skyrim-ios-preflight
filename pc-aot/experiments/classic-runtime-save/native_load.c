/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "native_load.h"
#include "rt.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static _Thread_local ClassicNativeLoad *armed;

static ClassicNativeLoad *current(const struct CPU *cpu) {
    return armed && armed->cpu == cpu && cpu == rt_current_cpu() ? armed : NULL;
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
    if (!runtime->pending_input || runtime->stream != stream ||
        !classic_runtime_load_generation(cpu, stream, runtime->handle)) return;
    (void)skse_bridge_bind_load_slot(runtime->pending_input, &load->event);
}

void classic_native_load_read_observed(const struct CPU *cpu, uint32_t stream,
                                      uint32_t entry_stack, uint32_t bytes) {
    ClassicNativeLoad *load = current(cpu);
    if (!load || !load->event.ticket) return;
    const int saved = errno;
    const SkyrimLifecycleCall slot = skyrim_lifecycle_scope_current();
    if (slot.site == SkyrimLoadSlot && slot.ticket == load->event.ticket && slot.generation == load->lifecycle.generation) {
        const uint64_t generation = classic_runtime_load_slot_read_end(cpu, stream, entry_stack, bytes);
        if (generation) classic_fresh_observe_read(cpu, stream, load->runtime.fd, generation, bytes);
    }
    errno = saved;
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
