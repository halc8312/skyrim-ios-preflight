/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PROBE_LOCAL_FILE_H
#define SKYRIM_EXPERIMENT_SKSE_PROBE_LOCAL_FILE_H

#include "plugin_trial.h"

typedef enum {
    SKSE_PROBE_FILE_CHECKED,
    SKSE_PROBE_FILE_UNAVAILABLE,
    SKSE_PROBE_FILE_UNSUPPORTED,
    SKSE_PROBE_FILE_TOO_LARGE,
    SKSE_PROBE_FILE_NO_MEMORY,
    SKSE_PROBE_FILE_INCOMPLETE
} SkseProbeFileStatus;

typedef struct {
    SkseTrialResult trial;
    SksePeReport imports;
} SkseLocalDllReport;

typedef struct {
    uint32_t guest_base;
    const SkseX86ReadView *read_views;
    size_t read_view_count;
    bool capture_papyrus_startup;
    const uint32_t *hook_probe_sites;
    size_t hook_probe_count;
    SkseTrialRecordProbe *record_probe; // Optional locally authored co-save evidence.
} SkseLocalProbeSetup;

/* The host can supply validated snapshots for a local compatibility test.
 * The default application file picker supplies no game data. */
SkseProbeFileStatus skse_probe_local_dll_with_setup(const char *path, uint32_t uptime_ms,
                                                    const SkseLocalProbeSetup *setup, SkseLocalDllReport *out);

/* Read one user-selected, coordinated source file. The result lives in memory.
 * Never follow links, write to the source or load host code from it. */
SkseProbeFileStatus skse_probe_local_dll(const char *path, uint32_t uptime_ms, SkseLocalDllReport *out);
#endif
