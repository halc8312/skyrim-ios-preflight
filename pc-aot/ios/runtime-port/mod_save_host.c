/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_save_host.h"
#include "save_attempt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
#include "rt.h"
#include "classic_mod_catalog.h"
#include "classic_papyrusutil.h"
#endif

/* Published before any guest thread starts. The profile descriptor stays
 * available to guest threads for this process's one runtime launch. */
static int launch_profile = -1;
static char mapped_saves[CLASSIC_RUNTIME_PATH_BYTES];
static _Thread_local ClassicSaveAttempt current_save;
static _Thread_local int current_directory = -1;
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
static _Thread_local SkyrimVmScope save_vm;
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
static bool read_catalog(void *unused, uint32_t address, void *bytes, size_t length) {
    (void)unused;
    if (!MEM || address < 0x10000u || length > UINT64_C(0x100000000) - address) return false;
    memcpy(bytes, MEM + address, length);
    return true;
}

static void observe_storage_records(void) {
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
    skse_plugin_record_set_close(records);
    classic_mod_catalog_free(catalog);
}
#endif

static void publication(void *unused, int directory, const char *leaf, int ess,
                        const SkyrimLifecycleScope *scope, uint32_t slot) {
    (void)unused;
    (void)directory;
    (void)leaf;
    (void)ess;
    (void)scope;
    (void)slot;
    fputs("IOS_CLASSIC_FRESH_SAVE published=1\n", stderr);
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
    observe_storage_records();
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
