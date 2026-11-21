/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_requirements.h"
#include <stdio.h>
#include <stdlib.h>

/* argv: root... ; prints "<status> <present> <expected>" */
int main(int argc, char **argv) {
    SkyrimSkyUIRequirements r;
    int e = skyrim_skyui_requirements((const char *const *)argv + 1, (size_t)(argc - 1), &r);
    if (e) { printf("error %d\n", e); return 0; }
    printf("%d %u %u\n", (int)r.status, r.support_present, r.support_expected);
    if (skyrim_skyui_requirements(NULL, 1, &r) == 0 || skyrim_skyui_requirements(NULL, 0, NULL) == 0) return 1;
    return 0;
}
