/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "roundtrip.h"
#include "event_bridge.h"
#include "save_paths.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

typedef enum { COMMIT, DEFER_BEFORE_STORE, SKIP_VM, FILE_FAILURE, RECORD_FAILURE, CLONE_FAILURE,
               INTERRUPT, STALE_INPUT, REPLACE } SaveMode;
static SaveMode mode;
static char directory[1024];
static SkseEventBridge *bridge;
static SkseGameEvent event = {
    .scope = {.launch = {1}, .profile = {2}},
};
static int held = -1, scratch = -1;
static struct stat at_publication;
static bool record_seen, finish_seen;
static SkyrimLifecycleScope lifecycle;

static void *try_foreign_return(void *argument) {
    const SkyrimLifecycleCall *foreign = argument;
    SkyrimLifecycleScope other;
    SkyrimLifecycleRow rows[2];
    assert(skyrim_lifecycle_scope_open(&other));
    SkyrimLifecycleCall slot = skyrim_lifecycle_begin(SkyrimSaveSlot);
    assert(slot.ticket == foreign->ticket);
    skyrim_lifecycle_end(*foreign, SkyrimNoResult);
    skyrim_lifecycle_end(slot, SkyrimNoResult);
    assert(skyrim_lifecycle_snapshot(rows, 2, NULL) == 2 && rows[1].returning && rows[1].ticket == slot.ticket);
    assert(skyrim_lifecycle_scope_close(&other));
    return NULL;
}

static void verify_scoped_lifecycle(void) {
    /* Exhaust the passive diagnostic prefix, then run more fresh attempts
     * than it could have witnessed. A delayed return cannot close a current
     * attempt, even when both local tickets have the same value. */
    for (unsigned i = 0; i < SkyrimLifecycleCapacity; ++i) {
        SkyrimLifecycleCall call = skyrim_lifecycle_begin(SkyrimRegisterPapyrus);
        skyrim_lifecycle_end(call, SkyrimNoResult);
    }
    assert(skyrim_lifecycle_scope_open(&lifecycle));
    SkyrimLifecycleCall slot = skyrim_lifecycle_begin(SkyrimSaveSlot);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, try_foreign_return, &slot));
    assert(!pthread_join(worker, NULL));
    skyrim_lifecycle_end(slot, SkyrimNoResult);
    assert(skyrim_lifecycle_scope_close(&lifecycle));
    assert(skyrim_lifecycle_scope_open(&lifecycle));
    SkyrimLifecycleCall cancelled = skyrim_lifecycle_begin(SkyrimSaveSlot);
    assert(!skyrim_lifecycle_scope_close(&lifecycle));
    for (unsigned i = 0; i < SkyrimLifecycleCapacity; ++i) {
        SkyrimLifecycleRow rows[4];
        uint64_t dropped;
        assert(skyrim_lifecycle_scope_open(&lifecycle));
        assert(!skyrim_lifecycle_scope_open(&lifecycle));
        SkyrimLifecycleCall slot = skyrim_lifecycle_begin(SkyrimSaveSlot);
        assert(slot.ticket == cancelled.ticket);
        skyrim_lifecycle_end(cancelled, SkyrimNoResult);
        SkyrimLifecycleCall vm = skyrim_lifecycle_begin(SkyrimSaveVM);
        skyrim_lifecycle_end(vm, SkyrimReturnedTrue);
        skyrim_lifecycle_end(slot, SkyrimNoResult);
        assert(skyrim_lifecycle_snapshot(rows, 4, &dropped) == 4 && !dropped);
        assert(rows[1].parent == slot.ticket && rows[2].outcome == SkyrimReturnedTrue &&
               rows[3].returning && rows[3].ticket == slot.ticket);
        assert(skyrim_lifecycle_scope_close(&lifecycle));
    }
    assert(skyrim_lifecycle_scope_open(&lifecycle));
    for (unsigned i = 0; i < SkyrimLifecycleCapacity / 2; ++i) {
        SkyrimLifecycleCall call = skyrim_lifecycle_begin(SkyrimRegisterPapyrus);
        skyrim_lifecycle_end(call, SkyrimNoResult);
    }
    assert(!skyrim_lifecycle_begin(SkyrimRegisterPapyrus).ticket);
    uint64_t dropped = 0;
    skyrim_lifecycle_snapshot(NULL, 0, &dropped);
    assert(dropped && !skyrim_lifecycle_scope_close(&lifecycle));
}

void save_roundtrip_configure(const char *name) {
    if (!strcmp(name, "committed")) mode = COMMIT;
    else if (!strcmp(name, "defer-before-store")) mode = DEFER_BEFORE_STORE;
    else if (!strcmp(name, "vm-rejected")) mode = SKIP_VM;
    else if (!strcmp(name, "short-write")) mode = FILE_FAILURE;
    else if (!strcmp(name, "stage-error")) mode = RECORD_FAILURE;
    else if (!strcmp(name, "clone-unavailable")) mode = CLONE_FAILURE;
    else if (!strcmp(name, "interrupted")) mode = INTERRUPT;
    else if (!strcmp(name, "replaced-before-capture")) mode = STALE_INPUT;
    else if (!strcmp(name, "replaced")) mode = REPLACE;
    else assert(0 && "Unrecognized roundtrip save case");
}

void save_composition_open(const char *root) {
    const size_t length = strlen(root);
    assert(length < sizeof directory && !bridge);
    memcpy(directory, root, length + 1);
    scratch = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(scratch >= 0);
    if (mode == DEFER_BEFORE_STORE) assert(!mkdirat(scratch, "text-en-voice-en", 0700));
    else {
        SksePairResult detail;
        assert(skse_bridge_open(root, &event.scope, &bridge, &detail) == SKSE_BRIDGE_OK);
    }
    verify_scoped_lifecycle();
    assert(skyrim_lifecycle_scope_open(&lifecycle));
}

static SkseEventTrace save_trace(SkyrimLifecycleRow rows[SkyrimLifecycleCapacity]) {
    SkseEventTrace trace = {.rows = rows};
    trace.count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &trace.dropped);
    memcpy(trace.launch, event.scope.launch, sizeof trace.launch);
    assert(trace.count && !trace.dropped);
    return trace;
}

void save_composition_begin(void) {
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    const SkseEventTrace trace = save_trace(rows);
    const SkyrimLifecycleRow *entry = &rows[trace.count - 1];
    assert(entry->site == SkyrimSaveSlot && !entry->parent && !entry->returning);
    event.ticket = entry->ticket;
}

void save_composition_record(void) {
    assert(event.ticket && !record_seen);
    record_seen = true;
}

void save_composition_finish(int published_fd, bool vm_saved) {
    assert(event.ticket && record_seen && !finish_seen && held < 0 && roundtrip_digest_bytes() == 0);
    assert(vm_saved == (mode != SKIP_VM));
    assert((published_fd < 0) == (mode == FILE_FAILURE));
    finish_seen = true;
    if (published_fd >= 0) {
        /* Only retain identity in the game callback. The bridge captures and
         * the worker hashes after the scope exits. */
        held = fcntl(published_fd, F_DUPFD_CLOEXEC, 0);
        assert(held >= 0 && !fstat(held, &at_publication));
    }
}

static void replace_saved_path(const char *bytes) {
    const size_t length = strlen(bytes);
    int fd = openat(scratch, "Replacement.ess", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    assert(fd >= 0 && write(fd, bytes, length) == (ssize_t)length && !close(fd));
    assert(!renameat(scratch, "Replacement.ess", scratch, "Fixture.ess"));
}

static void *publish_on_worker(void *arg) {
    SkseDeferredSave *job = arg;
    SkseGameScope changed = event.scope;
    ++changed.launch[0];
    SkseEventBridge *worker = NULL;
    SksePairResult detail;
    assert(skse_bridge_open(directory, &changed, &worker, &detail) == SKSE_BRIDGE_OK);
    assert(skse_bridge_publish_deferred(worker, job, &detail) == SKSE_BRIDGE_SCOPE);
    skse_bridge_close(worker);
    assert(roundtrip_digest_bytes() == 0);

    assert(skse_bridge_open(directory, &event.scope, &worker, &detail) == SKSE_BRIDGE_OK);
    assert(skse_bridge_publish_deferred(worker, job, &detail) == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK);
    assert(roundtrip_digest_bytes() == sizeof ROUNDTRIP_ORIGINAL - 1);
    assert(skse_bridge_publish_deferred(worker, job, &detail) == SKSE_BRIDGE_OK && detail == SKSE_PAIR_ALREADY);
    assert(roundtrip_digest_bytes() == sizeof ROUNDTRIP_ORIGINAL - 1);
    skse_bridge_drop_deferred(job);
    skse_bridge_close(worker);
    return NULL;
}

static SkseDeferredSave *capture_without_store(const SkseEventTrace *trace) {
    const int store = openat(scratch, "text-en-voice-en", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(store >= 0);
    static const uint8_t name[] = "Skyrim.esm";
    const SkseLoadedMod mod = {name, sizeof name - 1, 0, SKSE_LOADED_CLASSIC};
    const SksePairRecord record = {ROUNDTRIP_RECORD, ROUNDTRIP_RECORD_VERSION,
                                   sizeof ROUNDTRIP_RECORD_DATA, ROUNDTRIP_RECORD_DATA};
    const SksePublishedRecords records = {&mod, 1, ROUNDTRIP_OWNER, &record, 1};
    SksePairResult detail;
    SkseDeferredSave *job = NULL;
    assert(skse_bridge_defer_published_save(&event.scope, store, trace, event.ticket, scratch, "Alternate.ess",
        held, scratch, &records, &job, &detail) == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_ESS_CHANGED && !job);
    roundtrip_expect_clone(held, false);
    assert(skse_bridge_defer_published_save(&event.scope, store, trace, event.ticket, scratch, "Fixture.ess",
        held, scratch, &records, &job, &detail) == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK && job);
    assert(faccessat(store, "classic-generations.sqlite3", F_OK, 0) == -1 && errno == ENOENT);
    assert(!close(store));
    return job;
}

static SkseDeferredSave *capture_with_store(const SkseEventTrace *trace, bool matched, bool clones) {
    SkseDeferredSave *job = NULL;
    SksePendingSave *pending = NULL;
    SksePairResult detail;
    SkseBaseEvidence base;
    const SkseBridgeResult binding = skse_bridge_begin_published_save(bridge, trace, event.ticket,
        scratch, "Fixture.ess", held, &pending, &base, &detail);
    assert((pending != NULL) == (binding == SKSE_BRIDGE_OK));
    if (binding != SKSE_BRIDGE_OK) assert(base.ess_fd == -1 && !base.event.ticket);
    if (matched && mode != SKIP_VM) {
        assert(binding == SKSE_BRIDGE_OK && base.ess_fd == held && base.outcome == SKSE_BASE_SAVED);
        event = base.event;
        const SksePairRecord record = {ROUNDTRIP_RECORD, ROUNDTRIP_RECORD_VERSION,
                                       sizeof ROUNDTRIP_RECORD_DATA, ROUNDTRIP_RECORD_DATA};
        const bool bad_record = mode == RECORD_FAILURE;
        assert(skse_bridge_write_owner(pending, ROUNDTRIP_OWNER, bad_record ? NULL : &record, 1) ==
               (bad_record ? SKSE_PAIR_ARGUMENT : SKSE_PAIR_OK));
        if (clones) roundtrip_expect_clone(held, mode == CLONE_FAILURE);
        const SkseBridgeResult result = skse_bridge_defer_named_save(pending, trace, &base, scratch, "Fixture.ess",
                                                                     scratch, &job, &detail);
        if (mode == RECORD_FAILURE) assert(result == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_ARGUMENT);
        else if (mode == CLONE_FAILURE) assert(result == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_UNSUPPORTED);
        else assert(result == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK);
        assert((job != NULL) == (result == SKSE_BRIDGE_OK));
    } else if (mode == SKIP_VM) {
        assert(binding == SKSE_BRIDGE_REJECTED);
    } else if (held >= 0) {
        assert(binding == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_ESS_CHANGED);
    } else {
        assert(binding == SKSE_BRIDGE_ARGUMENT);
    }
    skse_bridge_drop(pending);
    return job;
}

void save_composition_close(void) {
    assert((bridge != NULL) == (mode != DEFER_BEFORE_STORE) && finish_seen && roundtrip_digest_bytes() == 0);
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    const SkseEventTrace trace = save_trace(rows);
    assert(skyrim_lifecycle_scope_close(&lifecycle));
    if (mode == STALE_INPUT) replace_saved_path(ROUNDTRIP_ORIGINAL);
    bool matched = false;
    if (held >= 0) {
        struct stat still_open, published;
        matched = !fstat(held, &still_open) && !fstatat(scratch, "Fixture.ess", &published, AT_SYMLINK_NOFOLLOW) &&
                  skse_save_same_version(&at_publication, &still_open) && skse_save_same_version(&still_open, &published);
    }
    assert(matched == (mode != FILE_FAILURE && mode != STALE_INPUT));
    const bool clones = mode == COMMIT || mode == DEFER_BEFORE_STORE || mode == INTERRUPT ||
                        mode == REPLACE || mode == CLONE_FAILURE;
    SkseDeferredSave *job = mode == DEFER_BEFORE_STORE ? capture_without_store(&trace) :
                                                         capture_with_store(&trace, matched, clones);
    roundtrip_finish_clone(clones);
    assert(roundtrip_digest_bytes() == 0);

    if (held >= 0) assert(!close(held));
    if (mode == REPLACE) replace_saved_path(ROUNDTRIP_REPLACEMENT);
    skse_bridge_close(bridge);
    assert(!close(scratch));
    if (mode == INTERRUPT) {
        skse_bridge_drop_deferred(job);
        assert(roundtrip_digest_bytes() == 0);
    } else if (job) {
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, publish_on_worker, job));
        assert(!pthread_join(worker, NULL));
    }
}
