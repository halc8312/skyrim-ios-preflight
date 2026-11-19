/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_DEPENDENCY_PLAN_H
#define SKYRIM_EXPERIMENT_SKSE_DEPENDENCY_PLAN_H

#include "pe_image.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SKSE_DEPENDENCY_MAX_HELPERS = 3, SKSE_DEPENDENCY_MAX_MODULES = 4, SKSE_DEPENDENCY_MAX_CATALOG = 16 };

typedef struct {
    const char *file_name; /* Basename only. The primary image can be anonymous. */
    const uint8_t *bytes;
    size_t length;
} SkseTrialDependency;

typedef enum {
    SKSE_DEPENDENCY_READY, SKSE_DEPENDENCY_MISSING, SKSE_DEPENDENCY_AMBIGUOUS,
    SKSE_DEPENDENCY_SELF, SKSE_DEPENDENCY_CYCLE, SKSE_DEPENDENCY_LIMIT,
    SKSE_DEPENDENCY_SIZE_LIMIT, SKSE_DEPENDENCY_UNREADABLE, SKSE_DEPENDENCY_INVALID_NAME,
    SKSE_DEPENDENCY_INVALID_IMAGE, SKSE_DEPENDENCY_ADDRESS_SPACE, SKSE_DEPENDENCY_NO_MEMORY
} SkseDependencyStatus;

typedef struct {
    SkseDependencyStatus status;
    SksePeStatus image_status;
    unsigned helper_count, direct_count, root_image_error;
    char load_order[SKSE_DEPENDENCY_MAX_HELPERS][SKSE_PE_MAX_IMPORT_NAME];
    char requester[SKSE_PE_MAX_IMPORT_NAME]; /* Empty denotes the selected plugin. */
    char requested[SKSE_PE_MAX_IMPORT_NAME];
} SkseDependencyTrace;

typedef struct {
    SksePeImage images[SKSE_DEPENDENCY_MAX_MODULES]; /* The primary module is always at zero. */
    unsigned count, helper_order[SKSE_DEPENDENCY_MAX_HELPERS];
    unsigned detach_steps[SKSE_DEPENDENCY_MAX_HELPERS]; /* Indices in helper_order. */
} SkseDependencySpace;

/* Read only caller-supplied byte buffers. Resolve a reachable DAG against its
 * package-local catalog and approved virtual service tokens. Map every import,
 * export and relocation before exposing any image to the interpreter. No path
 * is opened. Helpers attach depth-first by their PE import descriptors. An
 * unload releases a consumer before its providers, picking the first discovered
 * of equally eligible siblings. Pass a fresh output or close it before reuse. */
SkseDependencyStatus skse_dependency_map(SkseTrialDependency primary, const SkseTrialDependency *catalog, size_t count,
                                        uint32_t primary_base, const SksePeImportBinding *services, size_t service_count,
                                        SkseDependencySpace *out, SkseDependencyTrace *trace);
void skse_dependency_close(SkseDependencySpace *space);
int skse_trial_system_library(const char *name);
int skse_dependency_local_name_valid(const char *name);
#ifdef __cplusplus
}
#endif
#endif
