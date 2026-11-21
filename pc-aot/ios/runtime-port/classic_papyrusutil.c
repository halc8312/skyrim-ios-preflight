/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86.h"
#include "classic_papyrusutil.h"
#include "classic_vm_epoch.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { STORAGE_GET = SKYRIM_PAPYRUSUTIL_FUNCTIONS + 1, STORAGE_SET, ALL_FUNCTIONS = STORAGE_SET };

/* Types: 2 string, 3 int, 4 float, 5 bool (classic_settings signatures). */
static const ClassicValueFunction functions[ALL_FUNCTIONS] = {
    {"JsonUtil", "GetIntValue", 1, 3, 3, {2, 2, 3}},
    {"JsonUtil", "GetFloatValue", 2, 4, 3, {2, 2, 4}},
    {"JsonUtil", "GetStringValue", 3, 2, 3, {2, 2, 2}},
    {"JsonUtil", "SetIntValue", 4, 3, 3, {2, 2, 3}},
    {"JsonUtil", "SetFloatValue", 5, 4, 3, {2, 2, 4}},
    {"JsonUtil", "SetStringValue", 6, 2, 3, {2, 2, 2}},
    {"JsonUtil", "Save", 7, 5, 2, {2, 5}},
    {"JsonUtil", "Load", 8, 5, 1, {2}},
    {"JsonUtil", "IntListClear", 9, 3, 2, {2, 2}},
    {"JsonUtil", "IntListGet", 10, 3, 3, {2, 2, 3}},
    {"JsonUtil", "IntListResize", 11, 3, 4, {2, 2, 3, 3}},
    {"JsonUtil", "IntListSet", 12, 3, 4, {2, 2, 3, 3}},
    {"StorageUtil", "GetIntValue", STORAGE_GET, 3, 3, {CLASSIC_VALUES_FORM, 2, 3}},
    {"StorageUtil", "SetIntValue", STORAGE_SET, 3, 3, {CLASSIC_VALUES_FORM, 2, 3}},
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
    const CPU *owner;
    uint64_t ticket;
} call;

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

static bool copy_form_id(void *context, CPU *c, const ClassicValueFrame *frame, uint32_t address, uint32_t *identity) {
    (void)context;
    return backend.copy_form_id && call.active && c == call.owner && classic_value_same_frame(frame, &call.frame) &&
           still_current() && backend.copy_form_id(backend.context, c, frame, address, identity);
}

static bool owns_frame(void *context, CPU *c, uint64_t epoch, const ClassicValueFrame *frame) {
    (void)context; (void)epoch;
    if (!c || !frame) return false;
    if (!call.active) {
        /* The first check of a Run: the VM must be open, and this generation is the call's. */
        if (!skyrim_vm_scope_current(&call.scope) || call.scope.registry != frame->registry) {
            atomic_fetch_add_explicit(&refused_stale, 1, memory_order_relaxed);
            return false;
        }
        call.active = true;
        call.frame = *frame;
        call.owner = c;
        return true;
    }
    if (c != call.owner || !classic_value_same_frame(&call.frame, frame)) return false;
    if (still_current()) return true;
    atomic_fetch_add_explicit(&refused_stale, 1, memory_order_relaxed);
    return false;
}

static bool invoke(void *context, uint64_t epoch, uint32_t function, const ClassicValue *args, size_t count,
                   ClassicValue *result, char *text, size_t capacity) {
    (void)context; (void)epoch;
    const unsigned supported = backend.copy_form_id ? ALL_FUNCTIONS : SKYRIM_PAPYRUSUTIL_FUNCTIONS;
    if (!call.active || call.ticketed || !function || function > supported) return false;
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
    const size_t count = backend.copy_form_id ? ALL_FUNCTIONS : SKYRIM_PAPYRUSUTIL_FUNCTIONS;
    const ClassicValueRequest request = {
        .epoch = values_epoch, .functions = functions, .count = count,
        .read_guest = read_guest, .copy_string = copy_string,
        .copy_form_id = backend.copy_form_id ? copy_form_id : NULL, .owns_frame = owns_frame, .invoke = invoke,
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
    atomic_store(&published, count);
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
#include "mod_plugins.h"
#include "probe_asset_file.h"
#include <stdlib.h>

static const char wearable_owner[] = "Chesko_WearableLantern.esp";
enum { LANTERN_LOCAL_KEYWORD = 0x01d9a9, FORM_KEYWORD = 4 };

static bool is_wearable_owner(const uint8_t *name, size_t length) {
    return !skse_classic_mod_name_compare(name, length, (const uint8_t *)wearable_owner, sizeof wearable_owner - 1);
}

static bool may_override_keyword(int view, const SkseLoadedMod *mod) {
    const char *name = (const char *)mod->name;
    if (!skyrim_plugin_name_valid(name)) return true;
    char path[5 + 256 + 1];
    const int path_length = snprintf(path, sizeof path, "Data/%s", name);
    if (path_length < 0 || (size_t)path_length >= sizeof path) return true;
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (skse_probe_asset_read_prefix_unique(&view, path, 64 * 1024, &bytes, &length)) return true;
    SkyrimPluginHeader header;
    const int status = skyrim_plugin_parse_header(bytes, length, &header);
    free(bytes);
    if (status != SKYRIM_PLUGIN_HEADER_OK) return true;
    bool possible = false;
    const char *master = header.masters;
    for (unsigned i = 0; i < header.master_count; ++i) {
        const size_t n = strlen(master);
        if (is_wearable_owner((const uint8_t *)master, n)) { possible = true; break; }
        master += n + 1;
    }
    skyrim_plugin_header_free(&header);
    return possible;
}

static unsigned wearable_keyword_index(int view, const SkseLoadedMod *mods, size_t count) {
    if (!mods || count > SKSE_CLASSIC_MOD_LIMIT) return 0;
    size_t owner = 0;
    while (owner < count && !is_wearable_owner(mods[owner].name, mods[owner].length)) ++owner;
    if (owner == count || mods[owner].kind != SKSE_LOADED_CLASSIC) return 0;
    for (size_t i = owner + 1; i < count; ++i)
        if (may_override_keyword(view, &mods[i])) return 0;
    return mods[owner].index;
}

static bool enable_wearable_keyword(SkyrimPapyrusUtilSettings *scope, uint64_t world, unsigned index) {
    if (!index || index >= SKSE_CLASSIC_MOD_LIMIT) return false;
    const SkseLoadedMod owner = {(const uint8_t *)wearable_owner, sizeof wearable_owner - 1,
                                (uint8_t)index, SKSE_LOADED_CLASSIC};
    const uint32_t full_id = (uint32_t)index << 24 | LANTERN_LOCAL_KEYWORD;
    if ((scope->keyword_id && scope->keyword_id != full_id) ||
        skse_classic_storage_enable_keyword(scope->settings, scope->epoch, scope->storage_pex,
                                           scope->storage_pex_size, &owner) != SKSE_SETTINGS_OK) return false;
    scope->keyword_id = full_id;
    scope->keyword_world = world;
    return true;
}

static bool bind_wearable_keyword(SkyrimPapyrusUtilSettings *scope, uint64_t world,
                                  const SkseLoadedMod *mods, size_t count) {
    if (!scope->storage_pex || !world) return false;
    if (scope->keyword_world == world) return true;
    return enable_wearable_keyword(scope, world, wearable_keyword_index(scope->game_fd, mods, count));
}

static bool settings_invoke(void *context, unsigned method, const ClassicValue *args, size_t count,
                            ClassicValue *result, char *text, size_t capacity, uint64_t *ticket) {
    SkyrimPapyrusUtilSettings *scope = context;
    if (method == STORAGE_GET || method == STORAGE_SET) {
        if (count != 3 || args[0].type != CLASSIC_VALUES_FORM || scope->keyword_world != call.scope.epoch ||
            args[0].bits != scope->keyword_id || args[1].type != CLASSIC_VALUES_STRING || !args[1].string ||
            args[2].type != CLASSIC_VALUES_INT) return false;
        const SkseClassicFormSnapshot keyword = {.id = args[0].bits, .type = FORM_KEYWORD,
            .handle = UINT64_C(0xffff00000000) | args[0].bits};
        SkseClassicScalarResult value;
        if (skse_classic_storage_invoke_keyword(scope->settings, scope->epoch,
                method == STORAGE_GET ? SKSE_STORAGE_KEYWORD_GET_INT : SKSE_STORAGE_KEYWORD_SET_INT,
                &keyword, args[1].string, strlen(args[1].string) + 1, args[2].bits, scope->uptime_ms,
                &value, ticket, NULL) != SKSE_SETTINGS_OK) return false;
        *result = (ClassicValue){.type = value.type, .bits = value.bits};
        return true;
    }
    SkseClassicScalar scalars[CLASSIC_VALUES_ARGS];
    if (count > CLASSIC_VALUES_ARGS) return false;
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

static bool settings_copy_form(void *context, CPU *c, const ClassicValueFrame *frame, uint32_t address,
                               uint32_t *identity) {
    SkyrimPapyrusUtilSettings *scope = context;
    if (scope->keyword_world != call.scope.epoch || !scope->keyword_id || !still_current() ||
        !skse_classic_values_matches_static_form(c, frame, address, scope->keyword_id)) return false;
    *identity = scope->keyword_id;
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
                                      settings_copy_string, scope, scope->storage_pex ? settings_copy_form : NULL};
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

bool skyrim_papyrusutil_needs_initial_catalog(void) {
    return backend.copy_form_id == settings_copy_form;
}

static bool bind_initial_catalog(void *context) {
    const StorageCapture *capture = context;
    if (!current_capture(capture)) return false;
    return bind_wearable_keyword(backend.context, capture->expected.epoch, capture->mods, capture->count) &&
           current_capture(capture);
}

bool skyrim_papyrusutil_initial_catalog(const SkyrimVmScope *scope, const SkseLoadedMod *mods, size_t count) {
    if (!scope || scope->epoch != 1 || !skyrim_papyrusutil_needs_initial_catalog()) return false;
    StorageCapture capture = {*scope, mods, count, NULL};
    return skse_classic_values_with_provider(provider_epoch, scope->registry, bind_initial_catalog, &capture);
}

static bool storage_uptime(uint32_t *out) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return false;
    *out = (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
    return true;
}

static bool capture_storage(void *context) {
    StorageCapture *capture = context;
    if (!current_capture(capture)) return false;
    SkyrimPapyrusUtilSettings *scope = backend.context;
    const bool had_keyword = scope->keyword_id != 0;
    const bool bound = bind_wearable_keyword(scope, capture->expected.epoch, capture->mods, capture->count);
    if (!bound && had_keyword) return false;
    uint32_t uptime;
    if (!storage_uptime(&uptime)) return false;
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

typedef struct {
    SkyrimVmScope vm;
    const SkseLoadedMod *mods;
    size_t count;
    const SksePluginRecordSet *records;
    const SkseSerialResolvers *resolvers;
} StorageLoad;

static bool apply_storage(void *context) {
    const StorageLoad *load = context;
    uint32_t uptime;
    SkyrimPapyrusUtilSettings *scope = backend.context;
    const bool had_keyword = scope->keyword_id != 0;
    const bool bound = bind_wearable_keyword(scope, load->vm.epoch, load->mods, load->count);
    if ((!bound && had_keyword) || !storage_uptime(&uptime) || !skyrim_vm_commit_begin(&load->vm)) return false;
    SkseSettingsStatus status = load->records ?
        skse_classic_storage_restore(scope->settings, scope->epoch, load->mods, load->count,
                                     uptime, load->records, load->resolvers) :
        skse_classic_storage_reset(scope->settings, scope->epoch, load->mods, load->count, uptime);
    skyrim_vm_commit_end();
    return status == SKSE_SETTINGS_OK;
}

bool skyrim_papyrusutil_apply_storage(const SkyrimVmScope *scope, const SkseLoadedMod *mods, size_t count,
                                     const SksePluginRecordSet *records, const SkseSerialResolvers *resolvers) {
    if (!scope || (records && !resolvers) || backend.invoke != settings_invoke) return false;
    StorageLoad load = {*scope, mods, count, records, resolvers};
    return skse_classic_values_with_provider(provider_epoch, scope->registry, apply_storage, &load);
}
#endif
