/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_local_file.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    for (int i = 1; i < argc; ++i) {
        SkseLocalDllReport report;
        SkseProbeFileStatus status = skse_probe_local_dll(argv[i], 4427, &report);
        if (status != SKSE_PROBE_FILE_CHECKED) {
            printf("%u\n", (unsigned)status);
        } else if (report.trial.status == SKSE_TRIAL_COMPLETED) {
            printf("ready,%u,%s\n", report.trial.plugin_version, report.trial.name);
        } else {
            printf("stopped,%u,%s,%u\n", (unsigned)report.trial.status,
                   skse_pe_status_text(report.trial.image_status), (unsigned)report.imports.symbol_count);
        }
    }
}
