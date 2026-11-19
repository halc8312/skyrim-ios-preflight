/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include "test_trial_cases.h"

static unsigned char buffers[SKSE_DEPENDENCY_MAX_CATALOG + 1][64 * 1024];

static int arg_rejected(SkseTrialDependency primary, const SksePeImportBinding *services, size_t count,
                        SkseDependencyStatus expected) {
    SkseDependencySpace space;
    SkseDependencyTrace trace;
    if (skse_dependency_map(primary, NULL, 0, 0, services, count, &space, &trace) != expected || space.count) return 0;
    for (unsigned i = 0; i < SKSE_DEPENDENCY_MAX_MODULES; ++i) if (space.images[i].bytes) return 0;
    return 1;
}

static int check_arguments(SkseTrialDependency primary) {
    SksePeImportBinding services[] = {{"KERNEL32.dll", "FixtureA", 0xe0001000}, {"kernel32.DLL", "FixtureA", 0xe0001004}};
    if (!arg_rejected(primary, services, 2, SKSE_DEPENDENCY_INVALID_IMAGE)) return 1;
    services[1].symbol = "FixtureB";
    services[1].service_token = services[0].service_token;
    if (!arg_rejected(primary, services, 2, SKSE_DEPENDENCY_INVALID_IMAGE)) return 2;
    services[1].service_token += 4;
    char too_long[SKSE_PE_MAX_IMPORT_NAME + 1];
    memset(too_long, 'x', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    services[1].symbol = too_long;
    if (!arg_rejected(primary, services, 2, SKSE_DEPENDENCY_INVALID_IMAGE)) return 3;
    services[1] = (SksePeImportBinding){"A.dll", "FixtureB", 0xe0001004};
    if (!arg_rejected(primary, services, 2, SKSE_DEPENDENCY_INVALID_IMAGE)) return 4;
    primary.length = (size_t)SKSE_PE_MAX_FILE_BYTES + 1;
    if (!arg_rejected(primary, NULL, 0, SKSE_DEPENDENCY_SIZE_LIMIT)) return 5;
    puts("input_contracts=5");
    return 0;
}

static int read_fixture(const char *path, unsigned index, SkseTrialDependency *file) {
    const char *name = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    file->file_name = name;
    FILE *source = fopen(path, "rb");
    if (!source) return 1; /* Named but unreadable catalog entry. */
    file->length = fread(buffers[index], 1, sizeof buffers[index], source);
    int valid = !ferror(source) && fgetc(source) == EOF;
    fclose(source);
    if (valid && file->length) file->bytes = buffers[index];
    return valid;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc - 3 > SKSE_DEPENDENCY_MAX_CATALOG) return 2;
    int table = strcmp(argv[1], "table") == 0;
    int moved = strcmp(argv[1], "moved") == 0;
    int only_map = strcmp(argv[1], "map") == 0;
    int contracts = strcmp(argv[1], "contracts") == 0;
    if (!table && !moved && !only_map && !contracts && strcmp(argv[1], "report")) return 2;
    SkseTrialDependency files[SKSE_DEPENDENCY_MAX_CATALOG + 1] = {0};
    for (int i = 2; i < argc; ++i) if (!read_fixture(argv[i], (unsigned)(i - 2), &files[i - 2])) return 3;
    if (contracts) return check_arguments(files[0]);
    if (only_map) {
        SkseDependencySpace space;
        SkseDependencyTrace trace;
        skse_dependency_map(files[0], files + 1, (size_t)(argc - 3), 0, NULL, 0, &space, &trace);
        unsigned owned = 0;
        for (unsigned i = 0; i < SKSE_DEPENDENCY_MAX_MODULES; ++i) owned += space.images[i].bytes != NULL;
        printf("count=%u\nowned=%u\n", space.count, owned);
        skse_dependency_close(&space);
        return 0;
    }
    SkseTrialRuntime runtime = {.uptime_ms = 4192, .guest_base = moved ? UINT32_C(0x60000000) : 0};
    for (size_t i = 0; i < (table ? sizeof trial_cases / sizeof trial_cases[0] : 1); ++i) {
        SkseTrialResult result = skse_plugin_trial_with_package(files[0], files + 1, (size_t)(argc - 3), trial_cases[i], runtime);
        if (table) {
            if (result.status != SKSE_TRIAL_COMPLETED && result.status != SKSE_TRIAL_QUERY_DECLINED &&
                result.status != SKSE_TRIAL_LOAD_DECLINED) return 4;
            print_trial(i, result.status != SKSE_TRIAL_QUERY_DECLINED, result.status == SKSE_TRIAL_COMPLETED,
                        result.plugin_version, result.name, result.load.service_calls[0], result.load.service_calls[1]);
        } else {
            static const char *const reasons[] = {"ready", "missing", "ambiguous", "self", "cycle", "limit", "size",
                "unreadable", "name", "image", "address", "memory"};
            const SkseDependencyTrace *trace = &result.dependencies;
            if ((size_t)trace->status >= sizeof reasons / sizeof *reasons) return 5;
            printf("reason=%s\npe=%s\nphase=%u\nrequester=%s\nrequested=%s\nfailing=%s\ndirect=%u\norder=",
                   reasons[trace->status], skse_pe_status_text(trace->image_status), (unsigned)result.status,
                   trace->requester, trace->requested, result.failing_helper, trace->direct_count);
            for (unsigned j = 0; j < trace->helper_count; ++j) printf("%s%s", j ? "," : "", trace->load_order[j]);
            printf("\nattached=%u\ndetached=%u\nsequence=%u\nprimary_attach=%u\nquery_steps=%u\nname=%s\nversion=%u\n",
                   result.helpers_attached, result.helpers_detached, result.dependency_detach.eax,
                   result.attach.instructions, result.query.instructions, result.name, result.plugin_version);
        }
    }
    return 0;
}
