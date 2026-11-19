/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PROBE_DIRECTORY_H
#define SKYRIM_EXPERIMENT_SKSE_PROBE_DIRECTORY_H

#include "plugin_trial.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SKSE_PROBE_DIRECTORY_MAX_FILES = 16, SKSE_PROBE_DIRECTORY_MAX_ENTRIES = 1024,
       SKSE_PROBE_DIRECTORY_PATH_BYTES = 288 };

typedef enum {
    SKSE_PROBE_DIRECTORY_CHECKED, SKSE_PROBE_DIRECTORY_NO_PLUGINS,
    SKSE_PROBE_DIRECTORY_UNAVAILABLE, SKSE_PROBE_DIRECTORY_AMBIGUOUS_LAYOUT,
    SKSE_PROBE_DIRECTORY_FILE_LIMIT, SKSE_PROBE_DIRECTORY_ENTRY_LIMIT,
    SKSE_PROBE_DIRECTORY_SIZE_LIMIT, SKSE_PROBE_DIRECTORY_INCOMPLETE, SKSE_PROBE_DIRECTORY_NO_MEMORY
} SkseProbeDirectoryStatus;

typedef enum {
    SKSE_PROBE_DLL_NOT_PLUGIN, SKSE_PROBE_DLL_NOT_REGULAR, SKSE_PROBE_DLL_UNREADABLE,
    SKSE_PROBE_DLL_EMPTY, SKSE_PROBE_DLL_TOO_LARGE,
    SKSE_PROBE_DLL_STANDALONE, SKSE_PROBE_DLL_PAIRED,
    SKSE_PROBE_DLL_DEPENDENCY_MISSING, SKSE_PROBE_DLL_DEPENDENCY_AMBIGUOUS,
    SKSE_PROBE_DLL_DEPENDENCY_MULTIPLE, SKSE_PROBE_DLL_DEPENDENCY_UNREADABLE,
    SKSE_PROBE_DLL_DEPENDENCY_SELF, SKSE_PROBE_DLL_DEPENDENCY_INVALID, SKSE_PROBE_DLL_DEPENDENCY_CHAIN,
    SKSE_PROBE_DLL_DEPENDENCY_CYCLE, SKSE_PROBE_DLL_DEPENDENCY_LIMIT
} SkseProbeDirectoryFileStatus;

typedef struct {
    char relative_path[SKSE_PROBE_DIRECTORY_PATH_BYTES];
    char dependency_name[SKSE_PE_MAX_IMPORT_NAME];
    char other_dependency_name[SKSE_PE_MAX_IMPORT_NAME]; /* A second direct import, or the asking helper after a refusal. */
    SkseProbeDirectoryFileStatus status;
    SksePeStatus export_status; /* Meaningful for DLLs whose bytes were read. */
    unsigned is_plugin, external_library_count, trial_ran;
    SkseTrialResult trial; /* Valid only when trial_ran == 1. */
} SkseProbeDirectoryFile;

typedef struct {
    unsigned file_count, plugin_count;
    SkseProbeDirectoryFile files[SKSE_PROBE_DIRECTORY_MAX_FILES];
} SkseProbeDirectoryReport;

/* Inspect the selected coordinated directory, its SKSE/Plugins and Data/SKSE/Plugins.
 * Path-component matches are ASCII case-insensitive. A byte-ordered result includes
 * all selected DLL names, even those without the two Classic exports. The caller owns
 * the report. No URLs, sources, caches or external libraries are written or retained.
 * Nested paths and files are opened relative to their directory without following links.
 * At most 64 MiB of file inputs are read in total, separate from the bounded interpreter.
 * A batch-level error clears the report and performs no trials. NO_PLUGINS may still
 * contain file diagnostics. Use CHECKED plus each file's trial_ran and trial.status to
 * distinguish identifying an export, invoking a probe and completing its lifecycle. */
SkseProbeDirectoryStatus skse_probe_local_directory(const char *path, uint32_t uptime_ms,
                                                    SkseProbeDirectoryReport *out);

#ifdef __cplusplus
}
#endif
#endif
