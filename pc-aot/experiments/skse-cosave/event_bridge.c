/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "event_bridge.h"
#include "immutable_ess.h"
#include "save_paths.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct SkseEventBridge { SkseGameScope running; SksePairStore *store; int directory; };
struct SksePendingSave {
    SkseEventBridge *bridge;
    SksePairStage *stage;
    SkseGameEvent requested;
    int held_fd, edited, rejected;
    SksePairResult edit_result;
};
struct SkseDeferredSave {
    SkseGameEvent event;
    SksePairStage *stage;
    SkseImmutableEss *snapshot;
    int directory;
};
struct SkseLoadInput {
    SkseEventBridge *bridge;
    SkseGameEvent requested;
    SkseLoadScopeCurrent scope_current;
    uint64_t owner_generation;
    int held_fd;
    struct stat at_open;
};

static int has_id(const uint8_t id[16]) {
    static const uint8_t zero[16] = {0};
    return memcmp(id, zero, sizeof zero) != 0;
}

static int same_scope(const SkseGameScope *a, const SkseGameScope *b) {
    return !memcmp(a->launch, b->launch, sizeof a->launch) && !memcmp(a->profile, b->profile, sizeof a->profile)
        && a->text == b->text && a->voices == b->voices;
}

static SkseBridgeResult from_pair(SksePairResult result, SksePairResult *detail) {
    if (detail) *detail = result;
    return result == SKSE_PAIR_OK || result == SKSE_PAIR_ALREADY ? SKSE_BRIDGE_OK : SKSE_BRIDGE_PAIR;
}

static SkseBridgeResult request_matches(SkseEventBridge *bridge, const SkseGameEvent *requested) {
    if (!bridge || !requested || !requested->ticket || requested->ticket > SkyrimLifecycleCapacity ||
        !has_id(requested->saved.slot) || !has_id(requested->saved.event)) return SKSE_BRIDGE_ARGUMENT;
    if (!same_scope(&requested->scope, &bridge->running) ||
        memcmp(requested->saved.profile, bridge->running.profile, sizeof requested->saved.profile))
        return SKSE_BRIDGE_SCOPE;
    return SKSE_BRIDGE_OK;
}

SkseBridgeResult skse_bridge_open(const char *root, const SkseGameScope *running,
                                 SkseEventBridge **out, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!out) return SKSE_BRIDGE_ARGUMENT;
    *out = NULL;
    if (!root || !*root || !running || !has_id(running->launch) || !has_id(running->profile) ||
        running->text > 1 || running->voices > 1) return SKSE_BRIDGE_ARGUMENT;
    struct stat st;
    if (lstat(root, &st) || !S_ISDIR(st.st_mode)) return SKSE_BRIDGE_IO;
    char path[PATH_MAX];
    int length = snprintf(path, sizeof path, "%s/text-%s-voice-%s", root,
                          running->text ? "ja" : "en", running->voices ? "ja" : "en");
    if (length < 0 || (size_t)length >= sizeof path || (mkdir(path, 0700) && errno != EEXIST)) return SKSE_BRIDGE_IO;
    SkseEventBridge *bridge = calloc(1, sizeof *bridge);
    if (!bridge) return SKSE_BRIDGE_IO;
    bridge->directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (bridge->directory < 0) { free(bridge); return SKSE_BRIDGE_IO; }
    SkseBridgeResult status = from_pair(skse_pair_open(path, &bridge->store), detail);
    if (status == SKSE_BRIDGE_OK) { bridge->running = *running; *out = bridge; }
    else skse_bridge_close(bridge);
    return status;
}

void skse_bridge_close(SkseEventBridge *bridge) {
    if (bridge) { skse_pair_close(bridge->store); close(bridge->directory); free(bridge); }
}

static SkseBridgeResult bind_named_ess(SkseEventBridge *bridge,
    int saves, const char *leaf, int source, SkseGameEvent *event, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!event) return SKSE_BRIDGE_ARGUMENT;
    *event = (SkseGameEvent){0};
    const size_t stem = skse_save_stem(leaf);
    if (!bridge || !stem) return SKSE_BRIDGE_ARGUMENT;
    const int flags = fcntl(source, F_GETFL);
    struct stat directory, original, named, checked;
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(saves, &directory) ||
        !S_ISDIR(directory.st_mode) || fstat(source, &original) || !S_ISREG(original.st_mode) ||
        original.st_size <= 0) return SKSE_BRIDGE_ARGUMENT;
    if ((uint64_t)original.st_size > SKSE_PAIR_MAX_ESS) return from_pair(SKSE_PAIR_LIMIT, detail);
    if (fstatat(saves, leaf, &named, AT_SYMLINK_NOFOLLOW))
        return from_pair(errno == ENOENT ? SKSE_PAIR_ESS_CHANGED : SKSE_PAIR_STORAGE, detail);
    if (named.st_nlink != 1 || !skse_save_same_version(&original, &named) ||
        fstat(source, &checked) || !skse_save_same_version(&original, &checked))
        return from_pair(SKSE_PAIR_ESS_CHANGED, detail);
    SkseGameEvent bound = {.scope = bridge->running};
    if (!skse_pair_named_key(bound.scope.profile, leaf, stem + 4, &original, &bound.saved))
        return from_pair(SKSE_PAIR_STORAGE, detail);
    *event = bound;
    return SKSE_BRIDGE_OK;
}

SkseBridgeResult skse_bridge_bind_named_ess(SkseEventBridge *bridge, uint32_t ticket,
    int saves, const char *leaf, int source, SkseGameEvent *event, SksePairResult *detail) {
    if (!ticket || ticket > SkyrimLifecycleCapacity) {
        if (detail) *detail = SKSE_PAIR_OK;
        if (event) *event = (SkseGameEvent){0};
        return SKSE_BRIDGE_ARGUMENT;
    }
    const SkseBridgeResult status = bind_named_ess(bridge, saves, leaf, source, event, detail);
    if (status == SKSE_BRIDGE_OK) event->ticket = ticket;
    return status;
}

static SkseBridgeResult confirm_named_ess(SkseEventBridge *bridge, const SkseGameEvent *requested,
    int saves, const char *leaf, int source, SksePairResult *detail) {
    SkseGameEvent actual;
    SkseBridgeResult result = bind_named_ess(bridge, saves, leaf, source, &actual, detail);
    if (result != SKSE_BRIDGE_OK) return result;
    if (memcmp(&actual.saved, &requested->saved, sizeof actual.saved))
        return from_pair(SKSE_PAIR_ESS_CHANGED, detail);
    return SKSE_BRIDGE_OK;
}

SkseBridgeResult skse_bridge_begin_save(SkseEventBridge *bridge, const SkseGameEvent *requested,
                                       const SksePairGeneration *verified_base,
                                       SksePendingSave **out, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!out) return SKSE_BRIDGE_ARGUMENT;
    *out = NULL;
    SkseBridgeResult result = request_matches(bridge, requested);
    if (result != SKSE_BRIDGE_OK) return result;
    SksePendingSave *save = calloc(1, sizeof *save);
    if (!save) return SKSE_BRIDGE_IO;
    save->held_fd = -1;
    save->bridge = bridge;
    save->requested = *requested;
    result = from_pair(skse_pair_begin(bridge->store, &requested->saved, verified_base, &save->stage), detail);
    if (result == SKSE_BRIDGE_OK) *out = save;
    else free(save);
    return result;
}

SksePairResult skse_bridge_write_owner(SksePendingSave *save, uint32_t uid,
                                      const SksePairRecord *records, size_t count) {
    if (!save) return SKSE_PAIR_ARGUMENT;
    if (!save->stage || save->rejected || save->held_fd >= 0) return SKSE_PAIR_SEALED;
    save->edit_result = skse_pair_replace(save->stage, uid, records, count);
    if (save->edit_result == SKSE_PAIR_OK) save->edited = 1;
    return save->edit_result;
}

SksePairResult skse_bridge_write_plugins(SksePendingSave *save, SksePluginRegistry *plugins,
                                        size_t *participants) {
    if (participants) *participants = 0;
    if (!save || !plugins) return SKSE_PAIR_ARGUMENT;
    if (!save->stage || save->rejected || save->held_fd >= 0) return SKSE_PAIR_SEALED;
    size_t completed = 0;
    SksePairResult result = skse_plugins_stage(plugins, save->stage, &completed);
    if (result == SKSE_PAIR_BUSY) return result;
    save->edit_result = result;
    if (result == SKSE_PAIR_OK && completed) save->edited = 1;
    if (participants) *participants = completed;
    return result;
}

static SkseBridgeResult verify(const SkseGameEvent *requested, uint32_t site, const SkseEventTrace *trace,
                               const SkseBaseEvidence *base) {
    if (base) {
        if (!same_scope(&requested->scope, &base->event.scope)) return SKSE_BRIDGE_SCOPE;
        if (requested->ticket != base->event.ticket || memcmp(&requested->saved, &base->event.saved, sizeof requested->saved))
            return SKSE_BRIDGE_EVENT;
        if (base->outcome == SKSE_BASE_FAILED) return SKSE_BRIDGE_REJECTED;
    }
    SkseBridgeResult status = skse_bridge_check_call(requested, site, trace);
    if (status != SKSE_BRIDGE_OK) return status;
    SkseBaseOutcome required = site == SkyrimSaveSlot ? SKSE_BASE_SAVED : SKSE_BASE_LOADED;
    return base && base->outcome == required && base->ess_fd >= 0 ? SKSE_BRIDGE_OK : SKSE_BRIDGE_NEEDS_BASE;
}

static SkseBridgeResult capture_ready(SksePendingSave *save, const SkseEventTrace *trace,
                                     const SkseBaseEvidence *base, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!save) return SKSE_BRIDGE_ARGUMENT;
    if (!save->stage || save->rejected || save->held_fd >= 0) return SKSE_BRIDGE_STATE;
    SkseBridgeResult status = verify(&save->requested, SkyrimSaveSlot, trace, base);
    if (status == SKSE_BRIDGE_REJECTED) save->rejected = 1;
    if (status != SKSE_BRIDGE_OK) return status;
    if (save->edit_result != SKSE_PAIR_OK) return from_pair(save->edit_result, detail);
    return save->edited ? SKSE_BRIDGE_OK : SKSE_BRIDGE_NO_EDITS;
}

SkseBridgeResult skse_bridge_capture_save(SksePendingSave *save, const SkseEventTrace *trace,
                                         const SkseBaseEvidence *base, SksePairResult *detail) {
    SkseBridgeResult status = capture_ready(save, trace, base, detail);
    if (status != SKSE_BRIDGE_OK) return status;
    int fd = fcntl(base->ess_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return SKSE_BRIDGE_IO;
    SksePairGeneration generation = {save->requested.saved, fd};
    status = from_pair(skse_pair_pin(save->stage, &generation), detail);
    if (status == SKSE_BRIDGE_OK) save->held_fd = fd;
    else close(fd);
    return status;
}

static SkseBridgeResult defer_ready_save(SksePendingSave *save, int source, int scratch,
                                        SkseDeferredSave **out, SksePairResult *detail) {
    SkseDeferredSave *job = calloc(1, sizeof *job);
    if (!job) return SKSE_BRIDGE_IO;
    job->directory = fcntl(save->bridge->directory, F_DUPFD_CLOEXEC, 0);
    if (job->directory < 0) { free(job); return SKSE_BRIDGE_IO; }
    SkseBridgeResult status = from_pair(skse_ess_capture_immutable(source, scratch, &job->snapshot), detail);
    if (status != SKSE_BRIDGE_OK) { skse_bridge_drop_deferred(job); return status; }
    job->event = save->requested;
    job->stage = save->stage;
    save->stage = NULL;
    *out = job;
    return SKSE_BRIDGE_OK;
}

SkseBridgeResult skse_bridge_defer_save(SksePendingSave *save, const SkseEventTrace *trace,
                                       const SkseBaseEvidence *base, int scratch,
                                       SkseDeferredSave **out, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!out) return SKSE_BRIDGE_ARGUMENT;
    *out = NULL;
    SkseBridgeResult status = capture_ready(save, trace, base, detail);
    return status == SKSE_BRIDGE_OK ? defer_ready_save(save, base->ess_fd, scratch, out, detail) : status;
}

SkseBridgeResult skse_bridge_defer_named_save(SksePendingSave *save, const SkseEventTrace *trace,
    const SkseBaseEvidence *base, int saves, const char *leaf, int scratch,
    SkseDeferredSave **out, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!out) return SKSE_BRIDGE_ARGUMENT;
    *out = NULL;
    SkseBridgeResult status = capture_ready(save, trace, base, detail);
    if (status != SKSE_BRIDGE_OK) return status;
    status = confirm_named_ess(save->bridge, &save->requested, saves, leaf, base->ess_fd, detail);
    if (status != SKSE_BRIDGE_OK) return status;
    SkseDeferredSave *job = NULL;
    status = defer_ready_save(save, base->ess_fd, scratch, &job, detail);
    if (status != SKSE_BRIDGE_OK) return status;
    status = confirm_named_ess(save->bridge, &save->requested, saves, leaf, base->ess_fd, detail);
    if (status == SKSE_BRIDGE_OK) *out = job;
    else skse_bridge_drop_deferred(job);
    return status;
}
SkseBridgeResult skse_bridge_publish_deferred(SkseEventBridge *worker, SkseDeferredSave *job,
                                             SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!worker || !job) return SKSE_BRIDGE_ARGUMENT;
    if (!same_scope(&worker->running, &job->event.scope)) return SKSE_BRIDGE_SCOPE;
    struct stat here, origin;
    if (fstat(worker->directory, &here) || fstat(job->directory, &origin)) return SKSE_BRIDGE_IO;
    if (here.st_dev != origin.st_dev || here.st_ino != origin.st_ino) return SKSE_BRIDGE_SCOPE;
    return from_pair(skse_pair_commit_immutable(worker->store, job->stage, &job->event.saved, job->snapshot), detail);
}
void skse_bridge_drop_deferred(SkseDeferredSave *job) {
    if (job) {
        skse_pair_discard(job->stage);
        skse_ess_release_immutable(job->snapshot);
        close(job->directory);
        free(job);
    }
}

SkseBridgeResult skse_bridge_publish(SksePendingSave *save, const SkseGameScope *running, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!save || !running) return SKSE_BRIDGE_ARGUMENT;
    if (!same_scope(&save->requested.scope, running)) return SKSE_BRIDGE_SCOPE;
    if (save->held_fd < 0 || save->rejected) return SKSE_BRIDGE_STATE;
    SksePairGeneration generation = {save->requested.saved, save->held_fd};
    return from_pair(skse_pair_commit(save->bridge->store, save->stage, &generation), detail);
}

void skse_bridge_drop(SksePendingSave *save) {
    if (save) {
        if (save->held_fd >= 0) close(save->held_fd);
        skse_pair_discard(save->stage);
        free(save);
    }
}

static SkseBridgeResult prepare_load_input(SkseEventBridge *bridge, const SkseGameEvent *requested,
    int source, int scratch, SkseLoadInput **out, int *loader_fd, SksePairResult *detail) {
    SkseImmutableEss *snapshot = NULL;
    SkseBridgeResult status = from_pair(skse_ess_capture_immutable(source, scratch, &snapshot), detail);
    if (status != SKSE_BRIDGE_OK) return status;
    int held_fd = skse_ess_take_immutable_fd(snapshot);
    if (held_fd < 0) return SKSE_BRIDGE_IO;
    struct stat first;
    if (fstat(held_fd, &first)) { close(held_fd); return SKSE_BRIDGE_IO; }
    int guest_fd = fcntl(held_fd, F_DUPFD_CLOEXEC, 0);
    if (guest_fd < 0) { close(held_fd); return SKSE_BRIDGE_IO; }
    SkseLoadInput *input = malloc(sizeof *input);
    if (!input) { close(held_fd); close(guest_fd); return SKSE_BRIDGE_IO; }
    *input = (SkseLoadInput){.bridge = bridge, .requested = *requested, .held_fd = held_fd, .at_open = first};
    *out = input;
    *loader_fd = guest_fd;
    return SKSE_BRIDGE_OK;
}

SkseBridgeResult skse_bridge_prepare_load_input(SkseEventBridge *bridge, const SkseGameEvent *requested,
    int source, int scratch, SkseLoadInput **out, int *loader_fd, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (out) *out = NULL;
    if (loader_fd) *loader_fd = -1;
    if (!out || !loader_fd) return SKSE_BRIDGE_ARGUMENT;
    SkseBridgeResult status = request_matches(bridge, requested);
    if (status != SKSE_BRIDGE_OK) return status;
    return prepare_load_input(bridge, requested, source, scratch, out, loader_fd, detail);
}

static SkseBridgeResult prepare_named_load(SkseEventBridge *bridge, uint32_t ticket,
    int saves, const char *leaf, int source, int scratch, SkseGameEvent *event, SkseLoadInput **out,
    int *loader_fd, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (event) *event = (SkseGameEvent){0};
    if (out) *out = NULL;
    if (loader_fd) *loader_fd = -1;
    if (!event || !out || !loader_fd) return SKSE_BRIDGE_ARGUMENT;
    SkseGameEvent bound;
    SkseBridgeResult status = bind_named_ess(bridge, saves, leaf, source, &bound, detail);
    if (status != SKSE_BRIDGE_OK) return status;
    bound.ticket = ticket;
    if (ticket) {
        status = request_matches(bridge, &bound);
        if (status != SKSE_BRIDGE_OK) return status;
    }
    SkseLoadInput *input = NULL;
    int fd = -1;
    status = prepare_load_input(bridge, &bound, source, scratch, &input, &fd, detail);
    if (status != SKSE_BRIDGE_OK) return status;
    status = confirm_named_ess(bridge, &bound, saves, leaf, source, detail);
    if (status == SKSE_BRIDGE_OK) {
        *event = bound;
        *out = input;
        *loader_fd = fd;
    } else {
        close(fd);
        skse_bridge_drop_load_input(input);
    }
    return status;
}

SkseBridgeResult skse_bridge_prepare_named_load(SkseEventBridge *bridge, uint32_t ticket,
    int saves, const char *leaf, int source, int scratch, SkseGameEvent *event,
    SkseLoadInput **out, int *loader_fd, SksePairResult *detail) {
    if (!ticket || ticket > SkyrimLifecycleCapacity) {
        if (detail) *detail = SKSE_PAIR_OK;
        if (event) *event = (SkseGameEvent){0};
        if (out) *out = NULL;
        if (loader_fd) *loader_fd = -1;
        return SKSE_BRIDGE_ARGUMENT;
    }
    return prepare_named_load(bridge, ticket, saves, leaf, source, scratch, event, out, loader_fd, detail);
}

SkseBridgeResult skse_bridge_capture_named_load(SkseEventBridge *bridge, SkseLoadScopeCurrent scope_current,
    int saves, const char *leaf,
    int source, int scratch, SkseLoadInput **out, int *loader_fd, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (out) *out = NULL;
    if (loader_fd) *loader_fd = -1;
    if (!out || !loader_fd || !scope_current) return SKSE_BRIDGE_ARGUMENT;
    const SkyrimLifecycleCall owner = scope_current();
    if (!owner.generation || owner.ticket) return SKSE_BRIDGE_STATE;

    SkseGameEvent bound;
    const SkseBridgeResult status = prepare_named_load(bridge, 0, saves, leaf, source, scratch,
                                                       &bound, out, loader_fd, detail);
    if (status == SKSE_BRIDGE_OK) {
        (*out)->scope_current = scope_current;
        (*out)->owner_generation = owner.generation;
    }
    return status;
}

SkseBridgeResult skse_bridge_bind_load_slot(SkseLoadInput *input, SkseGameEvent *event) {
    if (!event) return SKSE_BRIDGE_ARGUMENT;
    *event = (SkseGameEvent){0};
    if (!input || !input->scope_current || input->requested.ticket) return SKSE_BRIDGE_STATE;
    const SkyrimLifecycleCall call = input->scope_current();
    if (call.generation != input->owner_generation) return SKSE_BRIDGE_SCOPE;
    if (call.site != SkyrimLoadSlot || !call.ticket || call.parent) return SKSE_BRIDGE_EVENT;
    input->requested.ticket = call.ticket;
    *event = input->requested;
    return SKSE_BRIDGE_OK;
}

void skse_bridge_drop_load_input(SkseLoadInput *input) {
    if (input) { close(input->held_fd); free(input); }
}

static SksePairResult check_open_input(SkseLoadInput *input) {
    struct stat now;
    if (fstat(input->held_fd, &now)) return SKSE_PAIR_STORAGE;
    return now.st_mode == input->at_open.st_mode && skse_ess_same_version(&now, &input->at_open)
        ? SKSE_PAIR_OK : SKSE_PAIR_ESS_CHANGED;
}

static SksePairResult check_consumed_input(SkseLoadInput *input, int fd) {
    SksePairResult status = check_open_input(input);
    if (status != SKSE_PAIR_OK) return status;
    struct stat consumed;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fstat(fd, &consumed)) return SKSE_PAIR_STORAGE;
    return (flags & O_ACCMODE) == O_RDONLY && skse_ess_same_version(&input->at_open, &consumed)
        ? SKSE_PAIR_OK : SKSE_PAIR_WRONG_GENERATION;
}

SkseBridgeResult skse_bridge_finish_load_input(SkseLoadInput **pending, const SkseEventTrace *trace,
    const SkseBaseEvidence *completed, void **companion, size_t *length, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (companion) *companion = NULL;
    if (length) *length = 0;
    if (!pending) return SKSE_BRIDGE_ARGUMENT;
    SkseLoadInput *input = *pending;
    if (!input) return SKSE_BRIDGE_STATE;
    *pending = NULL;
    if (!companion || !length) {
        skse_bridge_drop_load_input(input);
        return SKSE_BRIDGE_ARGUMENT;
    }
    SkseBridgeResult status = SKSE_BRIDGE_OK;
    if (input->scope_current) {
        const SkyrimLifecycleCall owner = input->scope_current();
        if (owner.generation != input->owner_generation) status = SKSE_BRIDGE_SCOPE;
        else if (owner.ticket || !input->requested.ticket) status = SKSE_BRIDGE_STATE;
    }
    if (status == SKSE_BRIDGE_OK) status = verify(&input->requested, SkyrimLoadSlot, trace, completed);
    if (status == SKSE_BRIDGE_OK) status = from_pair(check_consumed_input(input, completed->ess_fd), detail);
    if (status == SKSE_BRIDGE_OK) {
        SksePairGeneration generation = {input->requested.saved, input->held_fd};
        status = from_pair(skse_pair_export(input->bridge->store, &generation, companion, length), detail);
    }
    if (status == SKSE_BRIDGE_OK) {
        status = from_pair(check_open_input(input), detail);
        if (status != SKSE_BRIDGE_OK) {
            free(*companion);
            *companion = NULL;
            *length = 0;
        }
    }
    skse_bridge_drop_load_input(input);
    return status;
}

SkseBridgeResult skse_bridge_load(SkseEventBridge *bridge, const SkseGameEvent *requested,
                                 const SkseEventTrace *trace, const SkseBaseEvidence *base,
                                 uint32_t uid, SksePairRevert reset, SksePairVisitor visit,
                                 void *scratch, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (!uid || !reset || !visit) return SKSE_BRIDGE_ARGUMENT;
    SkseBridgeResult status = request_matches(bridge, requested);
    if (status == SKSE_BRIDGE_OK) status = verify(requested, SkyrimLoadSlot, trace, base);
    if (status != SKSE_BRIDGE_OK) { reset(scratch); return status; }
    SksePairGeneration generation = {requested->saved, base->ess_fd};
    return from_pair(skse_pair_restore(bridge->store, &generation, uid, reset, visit, scratch), detail);
}

SkseBridgeResult skse_bridge_open_loaded_core(SkseEventBridge *bridge, const SkseGameEvent *requested,
    const SkseEventTrace *trace, const SkseBaseEvidence *base, const SkseLoadedMod *current, size_t count,
    SkseCoreSnapshot **out, SkseModMapResult *map_status, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (map_status) *map_status = SKSE_MAP_SOURCE_ERROR;
    if (!out) return SKSE_BRIDGE_ARGUMENT;
    *out = NULL;
    if (!current && count) return SKSE_BRIDGE_ARGUMENT;
    SkseBridgeResult result = request_matches(bridge, requested);
    if (result == SKSE_BRIDGE_OK) result = verify(requested, SkyrimLoadSlot, trace, base);
    if (result != SKSE_BRIDGE_OK) return result;
    SksePairGeneration loaded = {requested->saved, base->ess_fd};
    SkseCoreSnapshotResult core = skse_core_snapshot_open(bridge->store, &loaded, current, count, out, detail, map_status);
    if (core == SKSE_CORE_SOURCE_ERROR) return SKSE_BRIDGE_PAIR;
    return core == SKSE_CORE_OK ? SKSE_BRIDGE_OK : SKSE_BRIDGE_CORE;
}

SkseBridgeResult skse_bridge_load_plugins(SkseEventBridge *bridge, const SkseGameEvent *requested,
                                         const SkseEventTrace *trace, const SkseBaseEvidence *base,
                                         SksePluginRegistry *plugins, const SkseLoadedMod *current, size_t count,
                                         SkseModMapResult *map_status, SksePairResult *detail) {
    if (detail) *detail = SKSE_PAIR_OK;
    if (map_status) *map_status = SKSE_MAP_SOURCE_ERROR;
    if (!plugins) return SKSE_BRIDGE_ARGUMENT;
    SkseBridgeResult status = request_matches(bridge, requested);
    if (status == SKSE_BRIDGE_OK) status = verify(requested, SkyrimLoadSlot, trace, base);
    if (status != SKSE_BRIDGE_OK) {
        SksePairResult reset = skse_plugins_reset_scratch(plugins);
        return reset == SKSE_PAIR_OK ? status : from_pair(reset, detail);
    }
    SksePairGeneration generation = {requested->saved, base->ess_fd};
    return from_pair(skse_plugins_load(plugins, bridge->store, &generation, current, count, map_status), detail);
}
