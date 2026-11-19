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
    SKSE_TRIAL_SERIALIZATION_FAULT
} SkseTrialStatus;

typedef struct {
    uint32_t skse_version, runtime_version, editor, release, plugin_handle;
    uint32_t expose_release, expose_handle;
} SkseTrialOptions;

typedef struct {
    uint32_t uptime_ms;
    uint32_t guest_base; // Zero selects the preferred address in the PE file.
    unsigned core_messages, static_tls, serialization;
    SkseWinAssets assets;
} SkseTrialRuntime;

typedef struct {
    SkseTrialStatus status;
    SksePeStatus image_status;
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
#endif
