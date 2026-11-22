/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_requirements.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* argv: root... ; prints "<status> <present> <expected>" */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--xpmse")) {
        SkyrimXpmseRequirements x;
        if (skyrim_xpmse_requirements((const char *const *)argv + 2, (size_t)(argc - 2), &x)) return 1;
        printf("%u %u %u %u %u %u %u %u\n", x.fnis_hkx32, x.fnis_hkx64, x.fnis_hkx_unknown,
               x.fnis_lists, x.fnis_plugin, x.skse_script, x.nioverride, x.unreadable);
        return 0;
    }
    SkyrimSkyUIRequirements r;
    int e = skyrim_skyui_requirements((const char *const *)argv + 1, (size_t)(argc - 1), &r);
    if (e) { printf("error %d\n", e); return 0; }
    printf("%d %u %u\n", (int)r.status, r.support_present, r.support_expected);
    if (skyrim_skyui_requirements(NULL, 1, &r) == 0 || skyrim_skyui_requirements(NULL, 0, NULL) == 0) return 1;
    return 0;
}
