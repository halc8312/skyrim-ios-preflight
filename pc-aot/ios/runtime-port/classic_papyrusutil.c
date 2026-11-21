/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86.h"
#include "classic_papyrusutil.h"
#include "classic_vm_epoch.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Types: 2 string, 3 int, 4 float, 5 bool (classic_settings signatures). */
static const ClassicValueFunction functions[SKYRIM_PAPYRUSUTIL_FUNCTIONS] = {
    {"JsonUtil", "GetIntValue", 1, 3, 3, {2, 2, 3}},
    {"JsonUtil", "GetFloatValue", 2, 4, 3, {2, 2, 4}},
    {"JsonUtil", "GetStringValue", 3, 2, 3, {2, 2, 2}},
    {"JsonUtil", "SetIntValue", 4, 3, 3, {2, 2, 3}},
    {"JsonUtil", "SetFloatValue", 5, 4, 3, {2, 2, 4}},
    {"JsonUtil", "SetStringValue", 6, 2, 3, {2, 2, 2}},
    {"JsonUtil", "Save", 7, 5, 2, {2, 5}},
};

static SkyrimPapyrusUtilBackend backend;
static atomic_ullong published, calls, refused_stale, commits, discards, errors;
#ifdef SKYRIM_CLASSIC_SETTINGS_BACKEND
static uint64_t provider_epoch;
#endif

/* One call at a time: the staged Run holds its busy flag for the whole call,
 * so this state needs no lock. A revert on another thread only changes the
 * generation; the call notices at its next check. */
static struct {
    bool active, ticketed, committing;
    SkyrimVmScope scope;
    ClassicValueFrame frame;
    uint64_t ticket;
} call;

static bool same_frame(const ClassicValueFrame *a, const ClassicValueFrame *b) {
    return a->sp == b->sp && a->base == b->base && a->registry == b->registry && a->stack_id == b->stack_id
        && a->result == b->result && a->state == b->state && a->object == b->object;
}

static bool still_current(void) {
    SkyrimVmScope now;
    return skyrim_vm_scope_current(&now) && now.epoch == call.scope.epoch && now.registry == call.scope.registry;
}

static bool read_guest(void *context, CPU *c, uint32_t address, void *bytes, size_t length) {
    (void)context;
    return backend.read_guest(backend.context, c, address, bytes, length);
}

static bool copy_string(void *context, CPU *c, uint32_t address, char *text, size_t capacity) {
    (void)context;
    return backend.copy_string(backend.context, c, address, text, capacity);
}

static bool owns_frame(void *context, CPU *c, uint64_t epoch, const ClassicValueFrame *frame) {
    (void)context; (void)c; (void)epoch;
    if (!call.active) {
        /* The first check of a Run: the VM must be open, and this generation is the call's. */
        if (!skyrim_vm_scope_current(&call.scope)) {
            atomic_fetch_add_explicit(&refused_stale, 1, memory_order_relaxed);
            return false;
        }
        call.active = true;
        call.frame = *frame;
        return true;
    }
    if (!same_frame(&call.frame, frame)) return false;
    if (still_current()) return true;
    atomic_fetch_add_explicit(&refused_stale, 1, memory_order_relaxed);
    return false;
}

static bool invoke(void *context, uint64_t epoch, uint32_t function, const ClassicValue *args, size_t count,
                   ClassicValue *result, char *text, size_t capacity) {
    (void)context; (void)epoch;
    if (!call.active || call.ticketed || !function || function > SKYRIM_PAPYRUSUTIL_FUNCTIONS) return false;
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
    uint64_t ticket = 0;
    if (!backend.invoke(backend.context, function, args, count, result, text, capacity, &ticket)) {
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return false;
    }
    call.ticketed = true;
    call.ticket = ticket;
    return true;
}

static bool commit(void *context, uint64_t epoch, uint32_t function) {
    (void)context; (void)epoch; (void)function;
    if (!call.active || !call.ticketed) return false;
    if (!skyrim_vm_commit_begin(&call.scope)) {
        /* Reverted since the call began: write neither the result nor the file. */
        atomic_fetch_add_explicit(&refused_stale, 1, memory_order_relaxed);
        return false;
    }
    if (!backend.commit(backend.context, call.ticket)) {
        skyrim_vm_commit_end();
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return false;
    }
    call.ticketed = false;
    call.committing = true;  /* held until the result is written (finished) */
    atomic_fetch_add_explicit(&commits, 1, memory_order_relaxed);
    return true;
}

static void discard(void *context, uint64_t epoch) {
    (void)context; (void)epoch;
    if (!call.ticketed) return;
    backend.discard(backend.context, call.ticket);
    call.ticketed = false;
    atomic_fetch_add_explicit(&discards, 1, memory_order_relaxed);
}

static void finished(void *context, uint64_t epoch) {
    (void)context; (void)epoch;
    if (call.ticketed) discard(NULL, 0);  /* a path that never reached commit or discard */
    if (call.committing) skyrim_vm_commit_end();
    memset(&call, 0, sizeof call);
}

int skyrim_papyrusutil_publish(const SkyrimPapyrusUtilBackend *source, uint64_t values_epoch) {
    if (!source || !source->invoke || !source->commit || !source->discard || !source->read_guest
            || !source->copy_string || !values_epoch || backend.invoke) return -1;
    backend = *source;
    const ClassicValueRequest request = {
        .epoch = values_epoch, .functions = functions, .count = SKYRIM_PAPYRUSUTIL_FUNCTIONS,
        .read_guest = read_guest, .copy_string = copy_string, .owns_frame = owns_frame, .invoke = invoke,
        .commit = commit, .discard = discard, .finished = finished, .context = &backend
    };
    if (!skse_classic_values_request(&request)) {
        backend = (SkyrimPapyrusUtilBackend){0};
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return -1;
    }
#ifdef SKYRIM_CLASSIC_SETTINGS_BACKEND
    provider_epoch = values_epoch;
#endif
    atomic_store(&published, SKYRIM_PAPYRUSUTIL_FUNCTIONS);
    return 0;
}

const ClassicValueFunction *skyrim_papyrusutil_functions(void) { return functions; }

void skyrim_papyrusutil_stats(SkyrimPapyrusUtilStats *out) {
    *out = (SkyrimPapyrusUtilStats){atomic_load(&published), atomic_load(&calls), atomic_load(&refused_stale),
                                    atomic_load(&commits), atomic_load(&discards), atomic_load(&errors)};
}

void skyrim_papyrusutil_report(void) {
    SkyrimPapyrusUtilStats s;
    skyrim_papyrusutil_stats(&s);
    fprintf(stderr, "IOS_PAPYRUSUTIL schema=1 published=%llu calls=%llu refused_stale=%llu commits=%llu "
            "discards=%llu errors=%llu\n", (unsigned long long)s.published, (unsigned long long)s.calls,
            (unsigned long long)s.refused_stale, (unsigned long long)s.commits, (unsigned long long)s.discards,
            (unsigned long long)s.errors);
}

#ifdef SKYRIM_CLASSIC_SETTINGS_BACKEND
static bool settings_invoke(void *context, unsigned method, const ClassicValue *args, size_t count,
                            ClassicValue *result, char *text, size_t capacity, uint64_t *ticket) {
    SkyrimPapyrusUtilSettings *scope = context;
    SkseClassicScalar scalars[3];
    if (count > 3) return false;
    for (size_t i = 0; i < count; ++i) {
        scalars[i] = (SkseClassicScalar){.type = args[i].type, .bits = args[i].bits};
        if (args[i].type == CLASSIC_VALUES_STRING) {
            scalars[i].text = args[i].string;
            scalars[i].text_size = strlen(args[i].string) + 1;
        }
    }
    SkseClassicScalarResult value;
    if (skse_classic_settings_invoke(scope->settings, scope->epoch, (SkseSettingsMethod)method, scalars, count,
                                     scope->uptime_ms, &value, ticket, NULL) != SKSE_SETTINGS_OK) return false;
    *result = (ClassicValue){.type = value.type, .bits = value.type == CLASSIC_VALUES_STRING ? 0 : value.bits};
    if (value.type == CLASSIC_VALUES_STRING) {
        const size_t length = strnlen(value.text, sizeof value.text);
        if (!text || length >= capacity || length == sizeof value.text) return false;
        memcpy(text, value.text, length + 1);
    }
    return true;
}

static bool settings_commit(void *context, uint64_t ticket) {
    SkyrimPapyrusUtilSettings *scope = context;
    return skse_classic_settings_commit(scope->settings, scope->epoch, ticket) == SKSE_SETTINGS_OK;
}

static void settings_discard(void *context, uint64_t ticket) {
    SkyrimPapyrusUtilSettings *scope = context;
    skse_classic_settings_discard(scope->settings, scope->epoch, ticket);
}

/* Guest memory is the runtime's 4 GiB reservation; its low 64 KiB is inaccessible. */
static bool guest_range(uint32_t address, size_t length) {
    return address >= 0x10000u && length && length <= UINT64_C(0x100000000) - address;
}

static bool settings_read(void *context, CPU *c, uint32_t address, void *bytes, size_t length) {
    (void)context; (void)c;
    if (!guest_range(address, length)) return false;
    memcpy(bytes, MEM + address, length);
    return true;
}

static bool settings_copy_string(void *context, CPU *c, uint32_t address, char *text, size_t capacity) {
    (void)context; (void)c;
    if (!capacity || !guest_range(address, 1)) return false;
    for (size_t i = 0; i < capacity && guest_range(address, i + 1); ++i) {
        text[i] = (char)MEM[address + i];
        if (!text[i]) return true;
    }
    return false;
}

void skyrim_papyrusutil_settings_backend(SkyrimPapyrusUtilSettings *scope, SkyrimPapyrusUtilBackend *out) {
    *out = (SkyrimPapyrusUtilBackend){settings_invoke, settings_commit, settings_discard, settings_read,
                                      settings_copy_string, scope};
}

typedef struct {
    SkyrimVmScope expected;
    const SkseLoadedMod *mods;
    size_t count;
    SksePluginRecordSet **out;
} StorageCapture;

static bool current_capture(const StorageCapture *capture) {
    SkyrimVmScope current;
    return skyrim_vm_scope_current(&current) && current.epoch == capture->expected.epoch &&
           current.registry == capture->expected.registry;
}

static bool capture_storage(void *context) {
    StorageCapture *capture = context;
    if (!current_capture(capture)) return false;
    SkyrimPapyrusUtilSettings *scope = backend.context;
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return false;
    const uint32_t uptime = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
    SksePluginRecordSet *records = NULL;
    if (skse_classic_storage_capture(scope->settings, scope->epoch, capture->mods, capture->count,
                                     uptime, &records) != SKSE_SETTINGS_OK)
        return false;
    if (!current_capture(capture)) {
        skse_plugin_record_set_close(records);
        return false;
    }
    *capture->out = records;
    return true;
}

bool skyrim_papyrusutil_capture_storage(const SkyrimVmScope *scope, const SkseLoadedMod *mods,
                                       size_t count, SksePluginRecordSet **out) {
    if (!scope || !out || *out || backend.invoke != settings_invoke) return false;
    StorageCapture capture = {*scope, mods, count, out};
    return skse_classic_values_with_provider(provider_epoch, scope->registry, capture_storage, &capture);
}
#endif
