/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PLUGIN_TRIAL_H
#define SKYRIM_EXPERIMENT_SKSE_PLUGIN_TRIAL_H

#include "x86_call.h"
#include "dependency_plan.h"

typedef enum {
    SKSE_TRIAL_COMPLETED,
    SKSE_TRIAL_IMAGE_REFUSED,
    SKSE_TRIAL_NO_MEMORY,
    SKSE_TRIAL_ATTACH_FAULT,
    SKSE_TRIAL_ATTACH_DECLINED,
    SKSE_TRIAL_QUERY_FAULT,
    SKSE_TRIAL_QUERY_DECLINED,
    SKSE_TRIAL_INVALID_INFO,
    SKSE_TRIAL_LOAD_FAULT,
    SKSE_TRIAL_LOAD_DECLINED,
    SKSE_TRIAL_DETACH_FAULT,
    SKSE_TRIAL_MESSAGE_FAULT,
    SKSE_TRIAL_DEPENDENCY_REFUSED,
    SKSE_TRIAL_DEPENDENCY_ATTACH_FAULT,
    SKSE_TRIAL_DEPENDENCY_ATTACH_DECLINED,
    SKSE_TRIAL_DEPENDENCY_DETACH_FAULT,
    SKSE_TRIAL_TLS_ATTACH_FAULT,
    SKSE_TRIAL_TLS_DETACH_FAULT,
    SKSE_TRIAL_SERIALIZATION_FAULT,
    SKSE_TRIAL_WORKER_FAULT,
    SKSE_TRIAL_TASK_FAULT, SKSE_TRIAL_PAPYRUS_FAULT, SKSE_TRIAL_ENGINE_HOOKS_OBSERVED
} SkseTrialStatus;

typedef struct {
    uint32_t skse_version, runtime_version, editor, release, plugin_handle;
    uint32_t expose_release, expose_handle;
} SkseTrialOptions;

typedef struct {
    uint32_t uptime_ms;
    uint32_t guest_base; // Zero selects the preferred address in the PE file.
    unsigned core_messages, static_tls, serialization, worker_thread, task_interface;
    SkseWinAssets assets;
    const char *module_file_name; // Optional basename for a standalone DLL.
    uint32_t papyrus_probe_epoch; // Opt-in, resident test session only.
    bool papyrus_fixture_int0; // Enables a shadow RegisterFunction for the authored int() fixture.
    bool capture_papyrus_startup; // Hosted one-shot trial accepts registrations without executing them.
    bool trace_known_windows_imports; // Temporary diagnostic fallback. Resident sessions always bind strictly.
    /* Optional host-only snapshots. Storage must be retained unchanged for
     * the trial or session, including unload. No MOD can declare a view. */
    const SkseX86ReadView *read_views;
    size_t read_view_count;
    const uint32_t *hook_probe_sites; // One-shot host observation in Load only.
    size_t hook_probe_count;
    /* Optional, Load only, one-shot trials: record code patch requests against a
     * read-only view of the game image instead of failing on them (plan X5). */
    SkseX86PatchInventory *patch_inventory;
} SkseTrialRuntime;

typedef struct {
    SkseTrialStatus status;
    SksePeStatus image_status;
    bool partial_windows_imports; // Reached initialization with named stop-only Windows bindings.
    unsigned papyrus_startup_registrations;
    uint32_t first_papyrus_callback;
    SkseX86Result attach, query, load, detach, message;
    SksePeStatus dependency_status;
    SkseX86Result dependency_attach, dependency_detach;
    uint32_t info_version, plugin_version, messages_delivered, unavailable_interface;
    char name[128];
    SkseDependencyTrace dependencies;
    unsigned helpers_attached, helpers_detached;
    char failing_helper[SKSE_PE_MAX_IMPORT_NAME];
    SkseX86Result tls_fault;
    uint32_t tls_callbacks;
    SkseX86Result serialization_fault;
    uint32_t serialization_callbacks, serialization_records;
    SkseX86Result worker_fault;
    unsigned workers_completed;
    SkseX86Result task_fault;
    unsigned tasks_completed[SKSE_X86_TASK_TYPES];
} SkseTrialResult;

/* Exercise the two cdecl exports in a private image. The only available
 * services are the two scalar version/handle methods. This is a probe,
 * with no access to the running game or to a save directory. */
SkseTrialResult skse_plugin_trial(const uint8_t *file, size_t length, SkseTrialOptions options);

/* Also interpret a PE entry point and the supported Win32/CRT imports.
 * Supply one uptime sample in milliseconds before the probe. The optional
 * core messages are the two data-free Classic startup notifications. */
SkseTrialResult skse_plugin_trial_with_runtime(const uint8_t *file, size_t length,
                                               SkseTrialOptions options, SkseTrialRuntime runtime);
/* The caller supplies a chosen dependency from the same package. Bind only
 * imports that are present by exact export name in this DLL. */
SkseTrialResult skse_plugin_trial_with_dependency(const uint8_t *file, size_t length,
                                                  SkseTrialOptions options, SkseTrialRuntime runtime,
                                                  SkseTrialDependency dependency);
/* Only reachable helpers from this package are considered. Catalog excludes
 * the primary. Duplicate names, cycles and the fourth helper are diagnosed. */
SkseTrialResult skse_plugin_trial_with_package(SkseTrialDependency primary, const SkseTrialDependency *catalog,
                                              size_t count, SkseTrialOptions options, SkseTrialRuntime runtime);

typedef struct SksePluginSession SksePluginSession;
typedef struct SksePluginRecordSet SksePluginRecordSet;
typedef enum {
    SKSE_SESSION_POST_LOAD, SKSE_SESSION_POST_POST_LOAD, SKSE_SESSION_GAME_TASKS,
    SKSE_SESSION_UI_TASKS, SKSE_SESSION_WORKER
} SkseSessionEvent;
typedef enum {
    SKSE_SESSION_OK, SKSE_SESSION_ARGUMENT, SKSE_SESSION_OUT_OF_ORDER, SKSE_SESSION_FAULTED,
    SKSE_SESSION_UNAVAILABLE, SKSE_SESSION_UID_MISMATCH, SKSE_SESSION_NO_MEMORY
} SkseSessionResult;

/* One isolated co-save trial. A supplied input is loaded before saving, and
 * otherwise the initial save is passed to the plugin's load callback. Use a
 * fresh NULL output slot. Success transfers ownership of the saved records
 * to the caller. This never enables a resident session or invokes game code.
 * A numeric hook observation remains an observation, regardless of replay. */
typedef struct {
    const SksePluginRecordSet *input;
    SksePluginRecordSet *output;
    SkseSessionResult status;
    uint32_t callback_work_limit; // Zero uses the default. The trial's total budget still applies.
} SkseTrialRecordProbe;

SkseTrialResult skse_plugin_trial_with_record_probe(const uint8_t *file, size_t length,
    SkseTrialOptions options, SkseTrialRuntime runtime, SkseTrialRecordProbe *probe);

/* Owns the validated images and all VM state after Query/Load succeeds. The
 * caller serializes calls and keeps any supplied asset callback context alive
 * until close. Module file buffers are needed only while opening the session. */
SksePluginSession *skse_plugin_session_open(SkseTrialDependency primary, const SkseTrialDependency *catalog,
                                            size_t count, SkseTrialOptions options, SkseTrialRuntime runtime,
                                            SkseTrialResult *report);
/* The two startup events are sent in order, once each. Queue events drain the
 * selected FIFO. A worker event runs any pending logical worker. Each event
 * has a fresh budget of 1 to 1,000,000 units. A fault stops later events. */
SkseSessionResult skse_plugin_session_step(SksePluginSession *session, SkseSessionEvent event,
                                         uint32_t work_limit, uint32_t uptime_ms);

/* Dispatch authored registration callbacks once against a private shadow
 * registry. This probe has no access to the engine or game Papyrus VM. */
SksePapyrusTrialReport skse_plugin_session_probe_papyrus(SksePluginSession *session, uint32_t epoch,
                                                       uint32_t work_limit, uint32_t uptime_ms,
                                                       SkseX86Result *callback_result);

/* Exercise an authored integer native in a fresh shadow frame. Requires
 * opt-in at open and a successful registration probe. The output changes
 * only after a successful typed result. No game VM object is involved. */
SksePapyrusTrialStatus skse_plugin_session_run_papyrus_fixture(SksePluginSession *session, uint32_t work_limit,
                                                              uint32_t uptime_ms, uint32_t *int_bits,
                                                              SkseX86Result *call_result);

/* Dispatch one registered Classic serialization callback after both startup
 * events. The caller determines the order of save, revert and load. Each gets
 * a fresh work budget and a fault ends the session's ability to accept events.
 * Save and load require a nonzero serialization UID.
 *
 * Pass an initialized NULL output slot to save. Success gives the caller an
 * immutable record set that outlives the session, including an empty set.
 * Load accepts a record set with the session's UID and uses its own reader.
 * Argument, ordering, availability, UID and allocation errors leave the
 * record set, output slot and session unchanged.
 */
SkseSessionResult skse_plugin_session_save(SksePluginSession *session, uint32_t work_limit,
                                          uint32_t uptime_ms, SksePluginRecordSet **output);
SkseSessionResult skse_plugin_session_revert(SksePluginSession *session, uint32_t work_limit,
                                            uint32_t uptime_ms);
SkseSessionResult skse_plugin_session_load(SksePluginSession *session, const SksePluginRecordSet *input,
                                          uint32_t work_limit, uint32_t uptime_ms);
/* Enable Classic FormID and handle resolution for this load callback only.
 * The caller pins the resolver and its catalog for the synchronous call. */
SkseSessionResult skse_plugin_session_load_resolved(SksePluginSession *session, const SksePluginRecordSet *input,
    const SkseSerialResolvers *resolvers, uint32_t work_limit, uint32_t uptime_ms);
uint32_t skse_plugin_record_set_uid(const SksePluginRecordSet *set);
uint32_t skse_plugin_record_set_count(const SksePluginRecordSet *set);

typedef struct {
    uint32_t type, version, length;
    const void *data;
} SksePluginRecord;
/* Read a borrowed record from an immutable set. Its data is valid until the
 * set is closed. The output remains unchanged on an invalid index. */
int skse_plugin_record_set_at(const SksePluginRecordSet *set, uint32_t index, SksePluginRecord *out);
/* Copy records restored by a host save reader into an independent set. Pass
 * a NULL-initialized output and a nonzero serialization UID. Invalid input
 * or allocation failure leaves the output unchanged. */
SkseSessionResult skse_plugin_record_set_create(uint32_t uid, const SksePluginRecord *records,
                                               uint32_t count, SksePluginRecordSet **out);
void skse_plugin_record_set_close(SksePluginRecordSet *set);

/* The report is borrowed for the session's lifetime and changes after events. */
const SkseTrialResult *skse_plugin_session_report(const SksePluginSession *session);
/* Run detach callbacks and discard pending work. Optionally copy the final report. */
void skse_plugin_session_close(SksePluginSession *session, SkseTrialResult *report);

const char *skse_trial_windows_service_name(uint32_t token);
#endif
