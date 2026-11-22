/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_save_host.h"
#include "save_attempt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "rt.h"
#include "classic_mod_catalog.h"
#include "classic_papyrusutil.h"
#include "copy_pair.h"
#include "event_bridge.h"
#include "interpreted_records.h"
#include "native_load.h"
#include "x86_skse_records.h"
#endif

/* Published before any guest thread starts. The profile descriptor stays
 * available to guest threads for this process's one runtime launch. */
static int launch_profile = -1;
static char mapped_saves[CLASSIC_RUNTIME_PATH_BYTES];
static _Thread_local ClassicSaveAttempt current_save;
static _Thread_local int current_directory = -1;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
static _Thread_local SkyrimVmScope save_vm;
static struct {
    SkseGameScope game;
    char root[CLASSIC_RUNTIME_PATH_BYTES];
    int root_directory, store_directory;
    dispatch_queue_t queue;
    atomic_uint pending;
} storage;
static _Thread_local struct {
    ClassicNativeLoad native;
    SkyrimLifecycleCall slot, vm;
    SkyrimVmScope accepted_vm;
    SkseEventBridge *bridge;
    int saves, scratch;
    char scratch_leaf[32];
} load_attempt;
#endif

bool skyrim_save_app_configure(const char *profile) {
    if (launch_profile >= 0 || !profile || *profile != '/') return false;
    const size_t size = strnlen(profile, sizeof mapped_saves);
    const char suffix[] = "/Documents/My Games/Skyrim/Saves";
    if (!size || size + sizeof suffix > sizeof mapped_saves || profile[size - 1] == '/') return false;
    const int root = open(profile, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) return false;
    memcpy(mapped_saves, profile, size);
    memcpy(mapped_saves + size, suffix, sizeof suffix);
    launch_profile = root;
    return true;
}

static int open_saves(void) {
    static const char *const components[] = {"Documents", "My Games", "Skyrim", "Saves"};
    int directory = launch_profile;
    for (size_t i = 0; i < sizeof components / sizeof *components; ++i) {
        const int child = openat(directory, components[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory != launch_profile) close(directory);
        if (child < 0) return -1;
        directory = child;
    }
    return directory;
}

#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
static int open_private_child(int directory, const char *leaf) {
    if (mkdirat(directory, leaf, 0700) && errno != EEXIST) return -1;
    const int opened = openat(directory, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (opened < 0) return -1;
    struct stat info;
    if (!fstat(opened, &info) && info.st_uid == geteuid() && !(info.st_mode & 0077)) return opened;
    close(opened);
    return -1;
}

bool skyrim_save_app_configure_storage(const char *profile, const uint8_t profile_id[16],
                                      unsigned text, unsigned voices) {
    if (launch_profile < 0 || storage.queue || !profile_id || !profile || text > 1 || voices > 1) return false;
    const int path_length = snprintf(storage.root, sizeof storage.root, "%s/ClassicCoSave", profile);
    if (path_length < 0 || (size_t)path_length >= sizeof storage.root) return false;
    const int root = open_private_child(launch_profile, "ClassicCoSave");
    if (root < 0) return false;
    static const char *const language_dirs[2][2] = {
        {"text-en-voice-en", "text-en-voice-ja"}, {"text-ja-voice-en", "text-ja-voice-ja"}
    };
    const int store = open_private_child(root, language_dirs[text][voices]);
    if (store < 0) { close(root); return false; }
    dispatch_queue_attr_t policy = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
    dispatch_queue_t queue = dispatch_queue_create("skyrim.classic.save", policy);
    if (!queue) { close(store); close(root); return false; }
    storage.game.text = text;
    storage.game.voices = voices;
    arc4random_buf(storage.game.launch, sizeof storage.game.launch);
    memcpy(storage.game.profile, profile_id, sizeof storage.game.profile);
    storage.root_directory = root;
    storage.store_directory = store;
    storage.queue = queue;
    return true;
}

typedef struct {
    const char *profile;
    const uint8_t *id;
    SkseEventBridge *source;
    bool copied;
} SaveCopyStorage;

static int copy_store_error(SkseBridgeResult result, SksePairResult detail) {
    if (result == SKSE_BRIDGE_OK) return 0;
    if (detail == SKSE_PAIR_BUSY) return EBUSY;
    if (detail == SKSE_PAIR_ESS_CHANGED || detail == SKSE_PAIR_WRONG_GENERATION) return ESTALE;
    if (detail == SKSE_PAIR_CONFLICT) return EEXIST;
    return EIO;
}

static int prepare_copy_storage(void *opaque, int source_dir, int original, const char *leaf, int staged) {
    SaveCopyStorage *copy = opaque;
    SksePairResult detail;
    SkseBridgeResult result = skse_bridge_check_copy_source(copy->source, source_dir, original, leaf, &detail);
    if (result != SKSE_BRIDGE_OK)
        return detail == SKSE_PAIR_MISSING ? 0 : copy_store_error(result, detail);
    if (!copy->id || !copy->profile) return ENOTSUP;
    char path[CLASSIC_RUNTIME_PATH_BYTES];
    int length = snprintf(path, sizeof path, "%s/ClassicCoSave", copy->profile);
    if (length < 0 || (size_t)length >= sizeof path) return ENAMETOOLONG;
    int profile = open(copy->profile, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (profile < 0) return errno;
    int root = open_private_child(profile, "ClassicCoSave");
    close(profile);
    if (root < 0) return EIO;
    SkseGameScope destination = storage.game;
    memcpy(destination.profile, copy->id, sizeof destination.profile);
    arc4random_buf(destination.launch, sizeof destination.launch);
    SkseEventBridge *bridge = NULL;
    result = skse_bridge_open(path, &destination, &bridge, &detail);
    if (result == SKSE_BRIDGE_OK)
        result = skse_bridge_prepare_copied_ess(copy->source, source_dir, original, bridge, staged, leaf, &detail);
    skse_bridge_close(bridge);
    close(root);
    copy->copied = result == SKSE_BRIDGE_OK;
    return copy_store_error(result, detail);
}

typedef struct {
    int root, error, copied;
    const char *source, *target, *leaf;
    SaveCopyStorage storage;
} SaveCopyTask;

static void copy_on_storage_queue(void *opaque) {
    SaveCopyTask *task = opaque;
    if (atomic_load_explicit(&storage.pending, memory_order_relaxed)) { task->error = EBUSY; return; }
    SaveCopyStorage *copy = &task->storage;
    SksePairResult detail;
    SkseBridgeResult result = skse_bridge_open_readonly(storage.root, &storage.game, &copy->source, &detail);
    task->error = copy_store_error(result, detail);
    if (!task->error) task->error = skse_copy_named_save_prepared(task->root, task->source, task->target,
        task->leaf, prepare_copy_storage, copy, &task->copied);
    if (!task->error && copy->copied) task->copied |= 2;
    skse_bridge_close(copy->source);
}

int skyrim_save_app_copy_selected(int root, const char *source, const char *target,
    const char *target_profile, const uint8_t target_id[16], const char *leaf, int *copied) {
    if (!storage.queue) return skse_copy_named_save(root, source, target, leaf, copied);
    if (!copied) return EINVAL;
    SaveCopyTask task = {.root = root, .source = source, .target = target, .leaf = leaf,
                         .storage = {.profile = target_profile, .id = target_id}};
    dispatch_sync_f(storage.queue, &task, copy_on_storage_queue);
    *copied = task.copied;
    return task.error;
}

static int open_scratch(char leaf[32]) {
    uint64_t nonce;
    arc4random_buf(&nonce, sizeof nonce);
    snprintf(leaf, 32, "work-%016llx", (unsigned long long)nonce);
    if (mkdirat(storage.root_directory, leaf, 0700)) return -1;
    const int descriptor = openat(storage.root_directory, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) unlinkat(storage.root_directory, leaf, AT_REMOVEDIR);
    return descriptor;
}

static void close_scratch(int scratch, const char *leaf) {
    if (scratch < 0) return;
    close(scratch);
    unlinkat(storage.root_directory, leaf, AT_REMOVEDIR);
}

static void publish_storage(void *context) {
    SkseDeferredSave *job = context;
    SkseEventBridge *worker = NULL;
    SksePairResult detail;
    SkseBridgeResult result = skse_bridge_open(storage.root, &storage.game, &worker, &detail);
    if (result == SKSE_BRIDGE_OK) result = skse_bridge_publish_deferred(worker, job, &detail);
    skse_bridge_drop_deferred(job);
    skse_bridge_close(worker);
    fprintf(stderr, "IOS_CLASSIC_SAVE_STORE phase=2 result=%u detail=%u\n", (unsigned)result, (unsigned)detail);
    atomic_fetch_sub_explicit(&storage.pending, 1, memory_order_relaxed);
}

static SkseBridgeResult queue_storage(int directory, const char *leaf, int ess, const SkyrimLifecycleScope *scope,
    uint32_t slot, const SkseLoadedMod *mods, size_t count, const SksePluginRecordSet *records, SksePairResult *detail) {
    if (!storage.queue) return SKSE_BRIDGE_STATE;
    unsigned pending = atomic_load_explicit(&storage.pending, memory_order_relaxed);
    do {
        if (pending >= 2) return SKSE_BRIDGE_STATE;
    } while (!atomic_compare_exchange_weak_explicit(&storage.pending, &pending, pending + 1,
                                                    memory_order_relaxed, memory_order_relaxed));
    SkseBridgeResult result = SKSE_BRIDGE_IO;
    const uint32_t size = skse_plugin_record_set_count(records);
    SksePairRecord pairs[SKSE_X86_RECORD_LIMIT];
    if (size > SKSE_X86_RECORD_LIMIT) result = SKSE_BRIDGE_ARGUMENT;
    for (uint32_t i = 0; i < size && result != SKSE_BRIDGE_ARGUMENT; ++i) {
        SksePluginRecord record;
        if (!skse_plugin_record_set_at(records, i, &record)) result = SKSE_BRIDGE_ARGUMENT;
        else pairs[i] = (SksePairRecord){record.type, record.version, record.length, record.data};
    }
    if (result != SKSE_BRIDGE_ARGUMENT) {
        char scratch_leaf[32];
        const int scratch = open_scratch(scratch_leaf);
        if (scratch >= 0) {
            SkseEventTrace trace = {.rows = scope->rows, .count = scope->count};
            memcpy(trace.launch, storage.game.launch, sizeof trace.launch);
            const SksePublishedRecords content = {mods, count, skse_plugin_record_set_uid(records), pairs, size};
            SkseDeferredSave *job = NULL;
            result = skse_bridge_defer_published_save(&storage.game, storage.store_directory, &trace, slot,
                directory, leaf, ess, scratch, &content, &job, detail);
            close_scratch(scratch, scratch_leaf);
            if (job) dispatch_async_f(storage.queue, job, publish_storage);
        }
    }
    if (result != SKSE_BRIDGE_OK) atomic_fetch_sub_explicit(&storage.pending, 1, memory_order_relaxed);
    return result;
}

static bool read_catalog(void *unused, uint32_t address, void *bytes, size_t length) {
    (void)unused;
    if (!MEM || address < 0x10000u || length > UINT64_C(0x100000000) - address) return false;
    memcpy(bytes, MEM + address, length);
    return true;
}

static void close_load(void) {
    classic_native_load_abort(&load_attempt.native);
    skse_bridge_close(load_attempt.bridge);
    close_scratch(load_attempt.scratch, load_attempt.scratch_leaf);
    close(load_attempt.saves);
    memset(&load_attempt, 0, sizeof load_attempt);
}

static uint64_t begin_load(const struct CPU *cpu) {
    if (!storage.queue || !cpu || cpu != rt_current_cpu()) return 0;
    if (load_attempt.bridge) {
        /* A nested call invalidates the prior proof without changing the guest. */
        (void)classic_native_load_enter(cpu);
        return 0;
    }
    const int saves = open_saves();
    if (saves < 0) return 0;
    SkseEventBridge *bridge = NULL;
    SksePairResult detail;
    const SkseBridgeResult result = skse_bridge_open_readonly(storage.root, &storage.game, &bridge, &detail);
    if (result != SKSE_BRIDGE_OK) { close(saves); return 0; }
    const int scratch = open_scratch(load_attempt.scratch_leaf);
    if (scratch < 0 || !classic_native_load_arm(&load_attempt.native, cpu, bridge, saves, mapped_saves, scratch)) {
        close_scratch(scratch, load_attempt.scratch_leaf);
        skse_bridge_close(bridge);
        close(saves);
        return 0;
    }
    load_attempt.bridge = bridge;
    load_attempt.saves = saves;
    load_attempt.scratch = scratch;
    const uint64_t ticket = classic_native_load_enter(cpu);
    if (!ticket) close_load();
    return ticket;
}

static bool apply_loaded_storage(const SkyrimVmScope *scope, const void *image, size_t length) {
    ClassicModCatalog *catalog = NULL;
    if (!scope->epoch ||
        classic_mod_catalog_capture(read_catalog, NULL, &catalog) != CLASSIC_MOD_CATALOG_OK) return false;
    SkseInterpretedModMap catalog_map = {0};
    catalog_map.current = classic_mod_catalog_entries(catalog, &catalog_map.current_count);
    SksePluginRecordSet *records = NULL;
    const bool matched = image &&
        skse_interpreted_decode(image, length, SKSE_CLASSIC_STORAGE_UID, &records, &catalog_map) == SKSE_PAIR_OK &&
        catalog_map.status == SKSE_MAP_OK;
    const SkseSerialResolvers resolvers = skse_interpreted_resolvers(&catalog_map.map);
    const bool applied = skyrim_papyrusutil_apply_storage(scope, catalog_map.current, catalog_map.current_count,
                                      matched ? records : NULL, matched ? &resolvers : NULL);
    skse_plugin_record_set_close(records);
    classic_mod_catalog_free(catalog);
    fprintf(stderr, "IOS_CLASSIC_LOAD_RECORDS matched=%u applied=%u\n", (unsigned)matched, (unsigned)applied);
    return applied;
}

static void finish_load(const struct CPU *cpu, uint64_t ticket, bool succeeded) {
    classic_fresh_leave(ticket, cpu, succeeded);
    void *image = NULL;
    size_t length = 0;
    SksePairResult detail;
    const SkseBridgeResult result = classic_native_load_finish(&load_attempt.native, &image, &length, &detail);
    if (result == SKSE_BRIDGE_OK || (result == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_MISSING))
        (void)apply_loaded_storage(&load_attempt.accepted_vm, image, length);
    free(image);
    fprintf(stderr, "IOS_CLASSIC_LOAD_STORE result=%u detail=%u\n", (unsigned)result, (unsigned)detail);
    close_load();
}

static uint64_t notify_load(const struct CPU *cpu, unsigned site, uint64_t ticket, uint32_t first, uint32_t second) {
    if (site == SkyrimFreshPluginsReady) {
        uint32_t data_handler;
        SkyrimVmScope initial;
        if (storage.queue && !ticket && !second && first && cpu && cpu == rt_current_cpu() &&
            skyrim_papyrusutil_needs_initial_catalog() &&
            skyrim_vm_scope_current(&initial) && initial.epoch == 1 &&
            read_catalog(NULL, 0x12e2cb8u, &data_handler, sizeof data_handler) && data_handler == first) {
            ClassicModCatalog *catalog = NULL;
            if (classic_mod_catalog_capture(read_catalog, NULL, &catalog) == CLASSIC_MOD_CATALOG_OK) {
                size_t count;
                const SkseLoadedMod *mods = classic_mod_catalog_entries(catalog, &count);
                (void)skyrim_papyrusutil_initial_catalog(&initial, mods, count);
                classic_mod_catalog_free(catalog);
            }
        }
        return 0;
    }
    if (site == SkyrimFreshBegin) return begin_load(cpu);
    if (!load_attempt.bridge || cpu != load_attempt.native.cpu || cpu != rt_current_cpu()) return 0;
    switch (site) {
    case SkyrimFreshReadBegin:
        return classic_native_load_read_begin(cpu, first);
    case SkyrimFreshReadLeave:
        classic_native_load_read_observed(cpu, first, (uint32_t)ticket, second);
        return 0;
    case SkyrimFreshVmBegin:
        if (load_attempt.vm.ticket) return 0;
        load_attempt.vm = classic_native_load_vm_begin(cpu, first);
        if (load_attempt.vm.site == SkyrimLoadVM) {
            load_attempt.accepted_vm = (SkyrimVmScope){0};
            (void)skyrim_vm_scope_pending_load(&load_attempt.accepted_vm);
        }
        return load_attempt.vm.ticket;
    case SkyrimFreshVmLeave:
        if (ticket == load_attempt.vm.ticket) {
            classic_native_load_vm_leave(cpu, load_attempt.vm, first);
            load_attempt.vm = (SkyrimLifecycleCall){0};
        }
        return 0;
    default:
        break;
    }
    if (!ticket || ticket != load_attempt.native.ticket) return 0;
    switch (site) {
    case SkyrimFreshCached: classic_fresh_cached(ticket, cpu); break;
    case SkyrimFreshOpenEnter: classic_native_load_open_enter(ticket, cpu, first, second); break;
    case SkyrimFreshOpenLeave: classic_native_load_open_leave(ticket, cpu, first != 0); break;
    case SkyrimFreshSlotEnter:
        load_attempt.slot = skyrim_lifecycle_begin(SkyrimLoadSlot);
        classic_native_load_slot_bind(ticket, cpu, first);
        classic_fresh_slot_enter(ticket, cpu, first);
        break;
    case SkyrimFreshSlotLeave:
        skyrim_lifecycle_end(load_attempt.slot, first ? SkyrimReturnedTrue : SkyrimReturnedFalse);
        classic_fresh_slot_leave(ticket, cpu, first != 0);
        break;
    case SkyrimFreshFinish: finish_load(cpu, ticket, first != 0); break;
    default: break;
    }
    return 0;
}

static uint64_t event_load(const struct CPU *cpu, unsigned site, uint64_t ticket, uint32_t first, uint32_t second) {
    const int saved = errno;
    const uint64_t result = notify_load(cpu, site, ticket, first, second);
    errno = saved;
    return result;
}

static void observe_storage_records(int directory, const char *leaf, int ess,
                                    const SkyrimLifecycleScope *scope, uint32_t slot) {
    if (!save_vm.epoch || current_save.runtime.owner != rt_current_cpu()) return;
    SkyrimVmScope now;
    if (!skyrim_vm_scope_current(&now) || now.epoch != save_vm.epoch || now.registry != save_vm.registry) return;
    ClassicModCatalog *catalog = NULL;
    if (classic_mod_catalog_capture(read_catalog, NULL, &catalog) != CLASSIC_MOD_CATALOG_OK) return;
    size_t count;
    const SkseLoadedMod *mods = classic_mod_catalog_entries(catalog, &count);
    SksePluginRecordSet *records = NULL;
    const bool captured = skyrim_papyrusutil_capture_storage(&save_vm, mods, count, &records);
    fprintf(stderr, "IOS_CLASSIC_SAVE_RECORDS captured=%u mods=%u records=%u\n",
            (unsigned)captured, (unsigned)count, skse_plugin_record_set_count(records));
    if (captured && storage.queue) {
        SksePairResult detail = SKSE_PAIR_OK;
        const SkseBridgeResult result = queue_storage(directory, leaf, ess, scope, slot, mods, count, records, &detail);
        fprintf(stderr, "IOS_CLASSIC_SAVE_STORE phase=1 result=%u detail=%u\n", (unsigned)result, (unsigned)detail);
    }
    skse_plugin_record_set_close(records);
    classic_mod_catalog_free(catalog);
}
#endif

static void publication(void *unused, int directory, const char *leaf, int ess,
                        const SkyrimLifecycleScope *scope, uint32_t slot) {
    (void)unused;
    fputs("IOS_CLASSIC_FRESH_SAVE published=1\n", stderr);
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    observe_storage_records(directory, leaf, ess, scope, slot);
#else
    (void)directory;
    (void)leaf;
    (void)ess;
    (void)scope;
    (void)slot;
#endif
}

static uint64_t begin_save(const struct CPU *cpu, uint32_t stack, uint32_t path) {
    if (launch_profile < 0 || current_directory >= 0) return 0;
    const int saved = errno;
    const int directory = open_saves();
    const uint64_t ticket = directory < 0 ? 0 : classic_save_attempt_begin_guest(
        &current_save, cpu, stack, path, directory, mapped_saves, publication, NULL);
    if (ticket) current_directory = directory;
    else if (directory >= 0) close(directory);
    errno = saved;
    return ticket;
}

static void close_save(void) {
    const int saved = errno;
    close(current_directory);
    current_directory = -1;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    save_vm = (SkyrimVmScope){0};
#endif
    errno = saved;
}

static void event_save(const struct CPU *cpu, uint64_t ticket, unsigned site, int succeeded) {
    if (current_directory < 0 || cpu != current_save.runtime.owner || ticket != current_save.runtime.token) return;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    if (site == SkyrimSaveSlotEntered) {
        save_vm = (SkyrimVmScope){0};
        (void)skyrim_vm_scope_current(&save_vm);
    }
#endif
    classic_save_attempt_event(cpu, ticket, site, succeeded);
    if (site == SkyrimSaveAttemptLeft) close_save();
}

void skyrim_save_app_fill(PcGuestModHooks *table) {
    table->save = (PcGuestModSaveHooks){begin_save, classic_save_attempt_current, event_save};
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    if (table->groups & PC_GUEST_MOD_HOOK_FRESH_LOAD) table->fresh_load = (PcGuestModFreshLoadHooks){event_load};
#endif
}

void skyrim_save_app_retire(const struct CPU *cpu) {
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    if (load_attempt.bridge && load_attempt.native.cpu == cpu) close_load();
#endif
    if (current_directory < 0 || cpu != current_save.runtime.owner) return;
    classic_save_attempt_cancel(cpu, current_save.runtime.token);
    close_save();
}
