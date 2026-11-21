/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "roundtrip.h"
#include "event_bridge.h"
#include "save_paths.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

typedef enum { COMMIT, SKIP_VM, FILE_FAILURE, RECORD_FAILURE, CLONE_FAILURE, INTERRUPT, STALE_INPUT, REPLACE } SaveMode;
static SaveMode mode;
static char directory[1024];
static SkseEventBridge *bridge;
static SksePendingSave *pending;
static SkseGameEvent event = {
    .scope = {.launch = {1}, .profile = {2}},
    .saved = {.profile = {2}, .slot = {3}, .event = {4}},
};
static int held = -1, scratch = -1;
static struct stat at_publication;
static bool finish_seen;

void save_roundtrip_configure(const char *name) {
    if (!strcmp(name, "committed")) mode = COMMIT;
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
    SksePairResult detail;
    assert(skse_bridge_open(root, &event.scope, &bridge, &detail) == SKSE_BRIDGE_OK);
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
    SksePairResult detail;
    assert(skse_bridge_begin_save(bridge, &event, NULL, &pending, &detail) == SKSE_BRIDGE_OK);
}

void save_composition_record(void) {
    const SksePairRecord record = {ROUNDTRIP_RECORD, ROUNDTRIP_RECORD_VERSION,
                                   sizeof ROUNDTRIP_RECORD_DATA, ROUNDTRIP_RECORD_DATA};
    const bool bad = mode == RECORD_FAILURE;
    const SksePairResult result = skse_bridge_write_owner(pending, ROUNDTRIP_OWNER, bad ? NULL : &record, 1);
    assert(result == (bad ? SKSE_PAIR_ARGUMENT : SKSE_PAIR_OK));
}

void save_composition_finish(int published_fd, bool vm_saved) {
    assert(pending && !finish_seen && held < 0 && roundtrip_digest_bytes() == 0);
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

void save_composition_close(void) {
    assert(bridge && pending && finish_seen && roundtrip_digest_bytes() == 0);
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    const SkseEventTrace trace = save_trace(rows);
    if (mode == STALE_INPUT) replace_saved_path(ROUNDTRIP_ORIGINAL);
    bool matched = false;
    if (held >= 0) {
        struct stat still_open, published;
        matched = !fstat(held, &still_open) && !fstatat(scratch, "Fixture.ess", &published, AT_SYMLINK_NOFOLLOW) &&
                  skse_save_same_version(&at_publication, &still_open) && skse_save_same_version(&still_open, &published);
    }
    assert(matched == (mode != FILE_FAILURE && mode != STALE_INPUT));
    const SkseBaseEvidence base = {event, matched ? SKSE_BASE_SAVED : SKSE_BASE_FAILED, matched ? held : -1};
    const bool clones = mode == COMMIT || mode == INTERRUPT || mode == REPLACE || mode == CLONE_FAILURE;
    if (clones) roundtrip_expect_clone(held, mode == CLONE_FAILURE);
    SkseDeferredSave *job = NULL;
    SksePairResult detail;
    const SkseBridgeResult result = skse_bridge_defer_save(pending, &trace, &base, scratch, &job, &detail);
    roundtrip_finish_clone(clones);
    if (mode == RECORD_FAILURE) assert(result == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_ARGUMENT);
    else if (mode == CLONE_FAILURE) assert(result == SKSE_BRIDGE_PAIR && detail == SKSE_PAIR_UNSUPPORTED);
    else if (!clones) assert(result == SKSE_BRIDGE_REJECTED);
    else assert(result == SKSE_BRIDGE_OK && detail == SKSE_PAIR_OK);
    assert((job != NULL) == (result == SKSE_BRIDGE_OK) && roundtrip_digest_bytes() == 0);

    if (held >= 0) assert(!close(held));
    if (mode == REPLACE) replace_saved_path(ROUNDTRIP_REPLACEMENT);
    skse_bridge_drop(pending);
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
