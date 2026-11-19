/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_directory.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2 && (argc != 3 || strcmp(argv[2], "repeat") != 0)) return 2;
    SkseProbeDirectoryReport report;
    SkseProbeDirectoryStatus status = skse_probe_local_directory(argv[1], 4427, &report);
    if (argc == 3) {
        for (unsigned i = 0; i < 128; ++i) {
            if (skse_probe_local_directory(argv[1], 4427, &report) != status) return 5;
        }
    }
    static const char *const directory_status[] = {"checked", "no-plugins", "unavailable", "ambiguous-layout",
        "file-limit", "entry-limit", "size-limit", "incomplete", "no-memory"};
    static const char *const file_status[] = {"not-plugin", "not-regular", "unreadable", "empty", "too-large",
        "standalone", "paired", "missing", "ambiguous", "multiple", "unreadable-dependency", "self", "invalid", "chain",
        "cycle", "dependency-limit"};
    if ((size_t)status >= sizeof directory_status / sizeof *directory_status) return 3;
    printf("%s|%u|%u\n", directory_status[status], report.file_count, report.plugin_count);
    for (unsigned i = 0; i < report.file_count; ++i) {
        const SkseProbeDirectoryFile *file = &report.files[i];
        if ((size_t)file->status >= sizeof file_status / sizeof *file_status) return 4;
        printf("%s|%s|%u|%u|%u|%s|%s|%s|%s|%u\n", file->relative_path, file_status[file->status],
               file->is_plugin, file->trial_ran, file->external_library_count, file->dependency_name,
               file->other_dependency_name, file->trial_ran && file->trial.status == SKSE_TRIAL_COMPLETED ? "yes" : "no",
               file->trial_ran ? skse_pe_status_text(file->trial.image_status) : "", file->trial.plugin_version);
    }
    return 0;
}
