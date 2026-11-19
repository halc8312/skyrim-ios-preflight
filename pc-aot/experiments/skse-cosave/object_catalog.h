/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "object_history.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t index, owning_stack, form_type;
    uint64_t saved_handle, mapped_handle;
    SkseModResolution resolution;
    SkseCoreName menu, target;
} SkseStoredUIForm;
typedef enum {
    SKSE_SPAWN_STORAGE, SKSE_SPAWN_QUEUE, SKSE_SPAWN_INCOMING, SKSE_SPAWN_WAITING, SKSE_SPAWN_READY
} SkseSpawnLocation;
typedef struct {
    SkseSpawnLocation location;
    uint32_t index, owning_stack, latent_stack, entries, has_delay;
    /* Incoming and waiting entries carry signed Windows performance-counter
     * ticks. Their raw bits are preserved; no clock frequency is in the file. */
    uint64_t delay_bits;
} SkseStoredSpawnTask;
typedef struct {
    uint32_t saved_form, mapped_form, target_ref_handle;
    SkseModResolution resolution;
    uint32_t position_bits[3], rotation_bits[3], count;
    uint8_t force_persist, initially_disabled;
} SkseStoredSpawnEntry;
typedef void (*SkseUIFormObserver)(void *, const SkseStoredUIForm *);
typedef void (*SkseSpawnTaskObserver)(void *, const SkseStoredSpawnTask *);
typedef void (*SkseSpawnEntryObserver)(void *, const SkseStoredSpawnTask *, uint32_t, const SkseStoredSpawnEntry *);
typedef struct {
    SkseObjectObserver mod_event;
    SkseArgumentObserver mod_argument;
    SkseUICallbackObserver ui_callback;
    SkseUIArgumentObserver ui_argument;
    SkseUIFormObserver ui_form;
    SkseSpawnTaskObserver spawn_task;
    SkseSpawnEntryObserver spawn_entry;
} SkseStoredObjectObservers;
typedef struct {
    uint32_t slots, filled, mod_events, ui_callbacks, ui_forms, spawn_tasks;
    uint32_t mod_arguments, ui_arguments, spawn_entries, process_local_strings;
    uint32_t unresolved_mod_forms, unresolved_ui_handles, unresolved_spawn_forms;
} SkseObjectCatalogSummary;
/* Inspect all four factories shipped by Classic 1.7.3. Unknown third-party
 * classes fail before any observer runs. All names and objects are borrowed
 * for the callback. An engine reference handle refers to a live game table;
 * it remains opaque even if the spawn form resolves successfully. */
SkseObjectResult skse_core_storage_objects(const SkseCoreSnapshot *snapshot, SkseObjectCatalogSummary *summary,
                                          const SkseStoredObjectObservers *observers, void *context);

typedef struct {
    uint32_t queued, incoming, waiting, ready, entries, unresolved_forms;
} SkseDelayedSummary;
/* Inspect DFMG queues of the built-in SpawnerTaskFunctor. index counts from
 * zero in each queue, and owning_stack applies only to OBMG storage. */
SkseObjectResult skse_core_delayed_tasks(const SkseCoreSnapshot *snapshot, SkseDelayedSummary *summary,
                                       SkseSpawnTaskObserver task, SkseSpawnEntryObserver entry, void *context);

#ifdef __cplusplus
}
#endif
