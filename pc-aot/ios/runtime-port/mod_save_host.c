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
#include "event_bridge.h"
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
        uint64_t nonce;
        arc4random_buf(&nonce, sizeof nonce);
        char scratch_leaf[32];
        snprintf(scratch_leaf, sizeof scratch_leaf, "work-%016llx", (unsigned long long)nonce);
        if (!mkdirat(storage.root_directory, scratch_leaf, 0700)) {
            const int scratch = openat(storage.root_directory, scratch_leaf,
                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (scratch >= 0) {
                SkseEventTrace trace = {.rows = scope->rows, .count = scope->count};
                memcpy(trace.launch, storage.game.launch, sizeof trace.launch);
                const SksePublishedRecords content = {mods, count, skse_plugin_record_set_uid(records), pairs, size};
                SkseDeferredSave *job = NULL;
                result = skse_bridge_defer_published_save(&storage.game, storage.store_directory, &trace, slot,
                    directory, leaf, ess, scratch, &content, &job, detail);
                close(scratch);
                if (job) dispatch_async_f(storage.queue, job, publish_storage);
            }
            unlinkat(storage.root_directory, scratch_leaf, AT_REMOVEDIR);
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
    static const char *const components[] = {"Documents", "My Games", "Skyrim", "Saves"};
    const int saved = errno;
    int directory = launch_profile;
    for (size_t i = 0; i < sizeof components / sizeof *components; ++i) {
        const int child = openat(directory, components[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory != launch_profile) close(directory);
        directory = child;
        if (directory < 0) break;
    }
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
}

void skyrim_save_app_retire(const struct CPU *cpu) {
    if (current_directory < 0 || cpu != current_save.runtime.owner) return;
    classic_save_attempt_cancel(cpu, current_save.runtime.token);
    close_save();
}
