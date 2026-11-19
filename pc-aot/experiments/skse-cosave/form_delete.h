/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "plugin_records.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t vm_epoch[16]; uint64_t sequence; } SkseDeletePosition;
typedef enum { SKSE_DELETE_OBSERVED, SKSE_DELETE_SLEEP_CLEARED, SKSE_DELETE_CORE_CLEARED } SkseDeletePhase;
typedef struct { SkseDeletePosition position; uint64_t handle; SkseDeletePhase phase; } SkseFormDeletion;
typedef struct {
    void *context;
    /* Query the active VM identity and its latest observed sequence.
     * Both callbacks belong to the VM owner thread. */
    int (*position)(void *, SkseDeletePosition *);
    /* Return the indicated original event. CORE_CLEARED is valid only after
     * the original VM sleep cleanup and all Classic core unregisters return. */
    int (*event)(void *, uint64_t requested_sequence, SkseFormDeletion *);
} SkseDeletionSource;
typedef enum {
    SKSE_DELETION_OK, SKSE_DELETION_ARGUMENT, SKSE_DELETION_BUSY, SKSE_DELETION_NOT_ARMED,
    SKSE_DELETION_OCCUPIED, SKSE_DELETION_PENDING, SKSE_DELETION_WRONG_WORLD, SKSE_DELETION_GAP,
    SKSE_DELETION_INCOMPLETE, SKSE_DELETION_SOURCE
} SkseDeletionResult;
typedef void (*SkseFormDeleteCallback)(void *plugin_state, uint64_t original_handle);
/* Form deletion is independent of owning serialized records. Only plugin
 * handles already in this registry can be assigned. NULL removes a callback. */
SksePairResult skse_plugins_set_form_delete(SksePluginRegistry *, uint32_t plugin, SkseFormDeleteCallback callback);
/* Start at the authoritative source's current sequence. A new VM needs a
 * fresh baseline. The source's context stays alive until stop or close. */
SkseDeletionResult skse_plugins_begin_deletions(SksePluginRegistry *, const SkseDeletionSource *);
SkseDeletionResult skse_plugins_stop_deletions(SksePluginRegistry *);
/* Call on an explicit VM notification. Deliver exactly the next confirmed
 * event, once and in ascending plugin-handle order. No file is consulted.
 * GAP requires recovering the exact missing evidence, or an explicit stop
 * and re-arm at a new baseline. The caller serializes epoch changes, VM core
 * cleanup and this synchronous dispatch on the owning thread. */
SkseDeletionResult skse_plugins_next_deletion(SksePluginRegistry *, size_t *notified);
#ifdef __cplusplus
}
#endif
