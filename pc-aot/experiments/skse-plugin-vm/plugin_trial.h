/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PLUGIN_TRIAL_H
#define SKYRIM_EXPERIMENT_SKSE_PLUGIN_TRIAL_H

#include "x86_call.h"

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
    SKSE_TRIAL_DETACH_FAULT
} SkseTrialStatus;

typedef struct {
    uint32_t skse_version, runtime_version, editor, release, plugin_handle;
    uint32_t expose_release, expose_handle;
} SkseTrialOptions;

typedef struct {
    uint32_t uptime_ms;
    uint32_t guest_base; // Zero selects the preferred address in the PE file.
} SkseTrialRuntime;

typedef struct {
    SkseTrialStatus status;
    SksePeStatus image_status;
    SkseX86Result attach, query, load, detach;
    uint32_t info_version, plugin_version;
    char name[128];
} SkseTrialResult;

/* Exercise the two cdecl exports in a private image. The only available
 * services are the two scalar version/handle methods. This is a probe,
 * with no access to the running game or to a save directory. */
SkseTrialResult skse_plugin_trial(const uint8_t *file, size_t length, SkseTrialOptions options);

/* Also interpret a PE entry point and the supported Win32/CRT imports.
 * Supply one uptime sample in milliseconds before the probe. */
SkseTrialResult skse_plugin_trial_with_runtime(const uint8_t *file, size_t length,
                                               SkseTrialOptions options, SkseTrialRuntime runtime);
#endif
