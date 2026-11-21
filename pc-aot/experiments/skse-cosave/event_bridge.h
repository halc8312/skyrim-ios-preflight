/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "generation.h"
#include "plugin_records.h"
#include "core_snapshot.h"
#include "observer/lifecycle.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SkseEventBridge SkseEventBridge;
typedef struct SksePendingSave SksePendingSave;
typedef struct SkseDeferredSave SkseDeferredSave;
typedef struct SkseLoadInput SkseLoadInput;

/* An independent process nonce and the running profile. Text and voices
 * are the effective game settings (0 English, 1 Japanese). */
typedef struct {
    uint8_t launch[16], profile[16];
    uint32_t text, voices;
} SkseGameScope;

/* ticket identifies this invocation in this process only. The save key must
 * come from durable, independently recoverable game-generation identity. */
typedef struct {
    SkseGameScope scope;
    SksePairKey saved;
    uint32_t ticket;
} SkseGameEvent;

typedef struct {
    uint8_t launch[16];
    const SkyrimLifecycleRow *rows;
    size_t count;
    uint64_t dropped;
} SkseEventTrace;

typedef enum {
    SKSE_BASE_UNCONFIRMED, SKSE_BASE_FAILED, SKSE_BASE_SAVED, SKSE_BASE_LOADED
} SkseBaseOutcome;
/* Only an authoritative game/storage coordinator may provide this outcome
 * and the exact finalized/consumed descriptor. A VM return cannot supply it. */
typedef struct {
    SkseGameEvent event;
    SkseBaseOutcome outcome;
    int ess_fd;
} SkseBaseEvidence;

typedef enum {
    SKSE_BRIDGE_OK, SKSE_BRIDGE_ARGUMENT, SKSE_BRIDGE_SCOPE, SKSE_BRIDGE_EVENT,
    SKSE_BRIDGE_TRACE, SKSE_BRIDGE_REJECTED, SKSE_BRIDGE_NEEDS_BASE,
    SKSE_BRIDGE_NO_EDITS, SKSE_BRIDGE_STATE, SKSE_BRIDGE_PAIR, SKSE_BRIDGE_IO, SKSE_BRIDGE_CORE
} SkseBridgeResult;

/* Validate the complete observed scope before any separate base-file proof.
 * An uncovered call shape or capacity loss is reported as an unproven trace. */
SkseBridgeResult skse_bridge_check_call(const SkseGameEvent *requested, uint32_t slot_site,
                                       const SkseEventTrace *trace);

/* The private root is provisioned and protected by the caller. The bridge
 * keeps a separate database for each effective game-language combination.
 * Children and their bridge are confined to one thread; drop children first. */
SkseBridgeResult skse_bridge_open(const char *private_root, const SkseGameScope *running,
                                 SkseEventBridge **out, SksePairResult *detail);
void skse_bridge_close(SkseEventBridge *bridge);
/* Derive a stable key for the exact named file version in an authorized saves
 * directory, from an independently witnessed descriptor. The caller passes
 * the canonical leaf selected by the game and borrows both descriptors. The
 * database still verifies the full base digest before handing out records. */
SkseBridgeResult skse_bridge_bind_named_ess(SkseEventBridge *bridge, uint32_t ticket,
    int saves, const char *leaf, int source, SkseGameEvent *event, SksePairResult *detail);
SkseBridgeResult skse_bridge_begin_save(SkseEventBridge *bridge, const SkseGameEvent *requested,
                                       const SksePairGeneration *verified_base,
                                       SksePendingSave **out, SksePairResult *detail);
/* Admit a fresh save witnessed by the actual writer. Check the closed save
 * and VM call trace before opening a stage for owner records. On success the
 * returned proof borrows source; use it with the same trace to capture. */
SkseBridgeResult skse_bridge_begin_published_save(SkseEventBridge *bridge, const SkseEventTrace *trace,
    uint32_t ticket, int saves, const char *leaf, int source,
    SksePendingSave **out, SkseBaseEvidence *proof, SksePairResult *detail);
SksePairResult skse_bridge_write_owner(SksePendingSave *pending, uint32_t uid,
                                      const SksePairRecord *records, size_t count);
/* Only the Classic host supplies the actual active catalog for this save. */
SksePairResult skse_bridge_write_mod_history(SksePendingSave *, const SkseLoadedMod *, size_t count);
SksePairResult skse_bridge_write_plugins(SksePendingSave *pending, SksePluginRegistry *plugins,
                                        size_t *participants);
SkseBridgeResult skse_bridge_capture_save(SksePendingSave *pending, const SkseEventTrace *trace,
                                         const SkseBaseEvidence *base, SksePairResult *detail);
SkseBridgeResult skse_bridge_publish(SksePendingSave *pending, const SkseGameScope *running,
                                    SksePairResult *detail);
void skse_bridge_drop(SksePendingSave *pending);

/* Capture an immutable, anonymous view of the confirmed .ess. No file
 * contents are read by this call. On success the pending stage is consumed
 * and the job may move to exactly one worker at a time. On unsupported
 * filesystems no job is created and the pending stage stays usable.
 * private_scratch must be a caller-managed, per-operation private directory
 * on the source volume. Apple requires that it contain no "base.clone". */
SkseBridgeResult skse_bridge_defer_save(SksePendingSave *pending, const SkseEventTrace *trace,
                                       const SkseBaseEvidence *base, int private_scratch,
                                       SkseDeferredSave **job, SksePairResult *detail);
/* Named variant: verify the original descriptor and directory entry before
 * and after immutable capture. A post-capture mismatch drops the consumed job. */
SkseBridgeResult skse_bridge_defer_named_save(SksePendingSave *pending, const SkseEventTrace *trace,
    const SkseBaseEvidence *base, int saves, const char *leaf, int private_scratch,
    SkseDeferredSave **job, SksePairResult *detail);
/* A worker opens its own bridge to the same protected store directory and
 * running scope. The first publish hashes the immutable file. Retries share
 * that digest. A job never shares a SQLite connection with the submitting
 * thread, and may outlive its original bridge. */
SkseBridgeResult skse_bridge_publish_deferred(SkseEventBridge *worker, SkseDeferredSave *job,
                                             SksePairResult *detail);
void skse_bridge_drop_deferred(SkseDeferredSave *job);

/* Start at an identified load's genuine input-open boundary, before any base
 * bytes are consumed. A successful capture returns an owned CLOEXEC read-only
 * descriptor. The loader must consume that immutable input and identify it
 * independently at completion. The scratch-directory contract matches
 * skse_bridge_defer_save(). Keep the bridge alive on the owner thread. */
SkseBridgeResult skse_bridge_prepare_load_input(SkseEventBridge *bridge, const SkseGameEvent *requested,
    int source, int private_scratch, SkseLoadInput **pending, int *loader_fd, SksePairResult *detail);
/* Prepare the exact selected named file before its first read. If identity is
 * lost when capturing it, return an empty pending input and keep source open. */
SkseBridgeResult skse_bridge_prepare_named_load(SkseEventBridge *bridge, uint32_t ticket,
    int saves, const char *leaf, int source, int private_scratch, SkseGameEvent *event,
    SkseLoadInput **pending, int *loader_fd, SksePairResult *detail);
/* For a loader that opens before the slot exists: capture under an idle
 * owner scope, give the loader the returned snapshot before any read, and
 * bind once while the genuine root load slot is active in that same scope.
 * Finish after the slot returns and before closing the scope. The caller
 * still proves that the original loader consumed the returned descriptor.
 * Supply the owning lifecycle observer even if it lives in a guest library. */
typedef SkyrimLifecycleCall (*SkseLoadScopeCurrent)(void);
SkseBridgeResult skse_bridge_capture_named_load(SkseEventBridge *bridge, SkseLoadScopeCurrent scope_current,
    int saves, const char *leaf,
    int source, int private_scratch, SkseLoadInput **pending, int *loader_fd, SksePairResult *detail);
SkseBridgeResult skse_bridge_bind_load_slot(SkseLoadInput *pending, SkseGameEvent *event);
/* One completion attempt consumes *pending. After the game and VM have
 * accepted the same immutable input and the stored generation digest agrees,
 * returns an owned Classic image for inspection. Caller frees it with free().
 * On error the result is NULL/zero. No plugin callbacks or VM changes occur. */
SkseBridgeResult skse_bridge_finish_load_input(SkseLoadInput **pending, const SkseEventTrace *trace,
    const SkseBaseEvidence *completed, void **companion, size_t *length, SksePairResult *detail);
void skse_bridge_drop_load_input(SkseLoadInput *pending);

/* This restores only private reversible scratch state. Discarding that state
 * cannot alter an active world. The caller owns any later VM handoff. */
SkseBridgeResult skse_bridge_load(SkseEventBridge *bridge, const SkseGameEvent *requested,
                                 const SkseEventTrace *trace, const SkseBaseEvidence *base,
                                 uint32_t uid, SksePairRevert reset, SksePairVisitor visit,
                                 void *scratch, SksePairResult *detail);
/* Open UID 0 only for the independently confirmed completed load. Caller
 * owns this immutable view until skse_core_snapshot_close(). */
SkseBridgeResult skse_bridge_open_loaded_core(SkseEventBridge *bridge, const SkseGameEvent *requested,
    const SkseEventTrace *trace, const SkseBaseEvidence *base, const SkseLoadedMod *current, size_t count,
    SkseCoreSnapshot **out, SkseModMapResult *map_status, SksePairResult *detail);
SkseBridgeResult skse_bridge_load_plugins(SkseEventBridge *bridge, const SkseGameEvent *requested,
                                         const SkseEventTrace *trace, const SkseBaseEvidence *base,
                                         SksePluginRegistry *plugins, const SkseLoadedMod *current, size_t count,
                                         SkseModMapResult *map_status, SksePairResult *detail);
#ifdef __cplusplus
}
#endif
