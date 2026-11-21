/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "mod_fuz_host.h"
#include "lift_resource_lookup.h"
#include "lift_world_seam.h"
#include "lift_prelude.h" /* CPU and MEM: the runtime's, or the hosted test's */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Trackers: one mutex, held only for memory reads in the reviewed helpers.
 * The resource lookup runs guest code and never holds it. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SkyrimFuzMenuTracker menu;
static SkyrimFuzWorldTracker world;
static SkyrimFuzPolicy policy;
static bool configured;
static atomic_ullong substituted, kept, missing, errors;
static atomic_llong last_report;

static bool in_guest(uint32_t address, size_t length) {
    return address >= SKYRIM_FUZ_GUEST_LOW_GUARD && length && length <= UINT64_C(0x100000000) - address;
}

static bool read_guest(void *context, uint32_t address, void *bytes, size_t length) {
    (void)context;
    if (!in_guest(address, length)) return false;
    memcpy(bytes, MEM + address, length);
    return true;
}

static bool replace_guest(void *context, uint32_t address, const void *expected, const void *replacement,
                          size_t length) {
    (void)context;
    if (!in_guest(address, length) || memcmp(MEM + address, expected, length)) return false;
    memcpy(MEM + address, replacement, length);
    return true;
}

static const SkyrimFuzLiftMemory memory = {read_guest, replace_guest, NULL};

int skyrim_fuz_app_configure(const SkyrimFuzSettings *settings) {
    if (!settings || configured || settings->words_per_second < 1) return -1;
    SkyrimFuzPolicy next = {.words_per_second = settings->words_per_second,
                            .skip_empty_responses = settings->skip_empty_responses != 0};
    for (unsigned n = 0; n < SKYRIM_FUZ_STOCK_COUNT; ++n) {
        next.stock_paths[n] = skyrim_fuz_stock_path(n + 1);
        if (!next.stock_paths[n]) return -1;
    }
    policy = next;
    configured = true;
    return 0;
}

bool skyrim_fuz_app_stack(uint32_t fs_base, uint32_t *limit, uint32_t *base) {
    if (fs_base < SKYRIM_FUZ_TEB_BASE || (fs_base - SKYRIM_FUZ_TEB_BASE) % SKYRIM_FUZ_TEB_STRIDE) return false;
    const uint32_t slot = (fs_base - SKYRIM_FUZ_TEB_BASE) / SKYRIM_FUZ_TEB_STRIDE;
    if (slot >= SKYRIM_FUZ_STACK_SLOTS) return false;
    uint32_t words[2];  /* TEB +4 StackBase, +8 StackLimit */
    if (!read_guest(NULL, fs_base + 4, words, sizeof words)) return false;
    const uint32_t lo = SKYRIM_FUZ_STACK_BASE + slot * SKYRIM_FUZ_STACK_SIZE;
    if (words[1] != lo || words[0] != lo + SKYRIM_FUZ_STACK_SIZE - SKYRIM_FUZ_STACK_TOP_RESERVE) return false;
    *limit = lo;
    *base = words[0];
    return true;
}

typedef struct {
    SkyrimFuzGuestResources resources;
    bool saw_missing_stock, saw_error;
} CountingLookup;

static SkyrimFuzAvailability counted_lookup(void *context, const char *path) {
    CountingLookup *lookup = context;
    const SkyrimFuzAvailability result = skyrim_fuz_guest_resource_lookup(&lookup->resources, path);
    if (result == SKYRIM_FUZ_LOOKUP_ERROR) lookup->saw_error = true;
    for (unsigned n = 0; result == SKYRIM_FUZ_ABSENT && n < SKYRIM_FUZ_STOCK_COUNT; ++n)
        if (!strcmp(path, policy.stock_paths[n])) lookup->saw_missing_stock = true;
    return result;
}

static void app_choose(const struct CPU *caller, uint32_t argument_slot, uint32_t response,
                       SkyrimFuzLiftChoice *choice) {
    static _Thread_local bool active;
    if (!configured || !choice || active) {
        if (active) atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return;
    }
    CountingLookup lookup = {.resources = {.caller = caller, .live_stack = argument_slot,
                                           .nested_bytes = SKYRIM_FUZ_NESTED_BYTES}};
    SkyrimFuzGuestLookupLayout layout;
    uint8_t probe[8];
    /* Same preconditions as skyrim_fuz_choose_with_guest_resources. */
    if (!caller || !skyrim_fuz_app_stack(caller->fs_base, &lookup.resources.stack_limit, &lookup.resources.stack_base)
            || !skyrim_fuz_guest_lookup_layout(&lookup.resources, &layout)
            || lookup.resources.stack_base - argument_slot < SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT + SKYRIM_FUZ_LIFT_FRAME_BYTES
            || !read_guest(NULL, response, probe, sizeof probe)) {
        atomic_fetch_add_explicit(&errors, 1, memory_order_relaxed);
        return;
    }
    SkyrimFuzPolicy counted = policy;
    counted.lookup = counted_lookup;
    counted.context = &lookup;
    active = true;
    const bool changed = skyrim_fuz_apply_lifted_path_choice(&counted, &memory, argument_slot, response, choice);
    active = false;
    atomic_ullong *counter = changed ? &substituted : lookup.saw_error ? &errors
                           : lookup.saw_missing_stock ? &missing : &kept;
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static void maybe_report(void) {
    struct timespec now;
    if (!configured || clock_gettime(CLOCK_MONOTONIC, &now)) return;
    long long previous = atomic_load_explicit(&last_report, memory_order_relaxed);
    if (now.tv_sec - previous < 60) return;
    if (atomic_compare_exchange_strong(&last_report, &previous, now.tv_sec)) skyrim_fuz_app_report(0);
}

static void app_forget(uint32_t response) {
    pthread_mutex_lock(&lock);
    skyrim_fuz_menu_forget(&menu, response);
    pthread_mutex_unlock(&lock);
}

static void app_note(uint32_t response, const SkyrimFuzLiftChoice *choice) {
    pthread_mutex_lock(&lock);
    skyrim_fuz_menu_assigned(&menu, &memory, response, choice);
    pthread_mutex_unlock(&lock);
    maybe_report();
}

static SkyrimFuzMenuScope app_capture_menu(uint32_t manager, uint32_t node, uint32_t title) {
    pthread_mutex_lock(&lock);
    const SkyrimFuzMenuScope scope = skyrim_fuz_menu_capture(&menu, &memory, manager, node, title);
    pthread_mutex_unlock(&lock);
    return scope;
}

static bool app_force_menu(const SkyrimFuzMenuScope *scope, uint32_t return_address, uint32_t title) {
    pthread_mutex_lock(&lock);
    const bool force = skyrim_fuz_menu_force(&menu, &memory, scope, return_address, title);
    pthread_mutex_unlock(&lock);
    return force;
}

static SkyrimFuzWorldScope app_capture_world(uint32_t response, uint32_t voice) {
    pthread_mutex_lock(&lock);
    const SkyrimFuzWorldScope scope = skyrim_fuz_world_capture(&menu, &memory, response, voice);
    pthread_mutex_unlock(&lock);
    maybe_report();
    return scope;
}

static bool app_admit_world(const SkyrimFuzWorldScope *scope, uint32_t return_address, uint32_t title) {
    pthread_mutex_lock(&lock);
    const bool admitted = skyrim_fuz_world_admit(&menu, &memory, scope, return_address, title);
    pthread_mutex_unlock(&lock);
    return admitted;
}

static void app_after_copy(uint32_t array, uint32_t index, uint32_t source, const SkyrimFuzWorldScope *scope) {
    pthread_mutex_lock(&lock);
    skyrim_fuz_world_after_copy(&world, &menu, &memory, array, index, source, scope);
    pthread_mutex_unlock(&lock);
}

static void app_release_title(uint32_t title) {
    pthread_mutex_lock(&lock);
    skyrim_fuz_world_release_title(&world, title);
    pthread_mutex_unlock(&lock);
}

static bool app_world_general(uint32_t array, uint32_t record) {
    pthread_mutex_lock(&lock);
    const bool shown = skyrim_fuz_world_general(&world, &memory, array, record);
    pthread_mutex_unlock(&lock);
    return shown;
}

void skyrim_fuz_app_retire(void *context, uint64_t old_epoch) {
    (void)context; (void)old_epoch;
    pthread_mutex_lock(&lock);
    const uint64_t issued = menu.issued;  /* serials stay unique across generations */
    menu = (SkyrimFuzMenuTracker){.issued = issued};
    world = (SkyrimFuzWorldTracker){0};
    pthread_mutex_unlock(&lock);
}

void skyrim_fuz_app_fill(PcGuestModHooks *table) {
    table->fuz_menu = (PcGuestModFuzMenuHooks){app_forget, app_choose, app_note, app_capture_menu, app_force_menu};
    table->fuz_world = (PcGuestModFuzWorldHooks){app_capture_world, app_admit_world, app_after_copy,
                                                 app_release_title, app_world_general};
}

void skyrim_fuz_app_counts(SkyrimFuzHostCounts *out) {
    *out = (SkyrimFuzHostCounts){atomic_load(&substituted), atomic_load(&kept), atomic_load(&missing),
                                 atomic_load(&errors)};
}

void skyrim_fuz_app_report(int final) {
    (void)final;
    SkyrimFuzHostCounts c;
    skyrim_fuz_app_counts(&c);
    fprintf(stderr, "IOS_FUZ schema=1 enabled=%u substituted=%llu kept=%llu missing=%llu errors=%llu\n",
            configured ? 1u : 0u, (unsigned long long)c.substituted, (unsigned long long)c.kept,
            (unsigned long long)c.missing, (unsigned long long)c.errors);
}
