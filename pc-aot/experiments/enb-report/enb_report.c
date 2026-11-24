/* SPDX-License-Identifier: GPL-3.0-only
 * Report what the ENB-compatible post-processing makes of an owner's preset
 * folder: which effect files exist, how many techniques, passes and UI
 * variables, which engine names, includes and SM4 syntax they use, and the pass
 * plan for each quality. Read-only; prints to stdout on the owner's machine.
 */
#define _GNU_SOURCE
#include "enb_preset.h"
#include "mod_plugins.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const names[ENB_EFFECT_COUNT] = {"adaptation", "bloom", "lens", "prepass", "depthoffield", "main", "postpass"};
static const char *const kinds[] = {"", "SM4 syntax", "include", "unsupported name", "limit", "no technique"};

static void report_effect(const char *directory, EnbEffect e, const EnbEffectInfo *info) {
    printf("  %-13s %s%s%s\n", names[e], info->present ? "present" : "absent", info->present && !info->enabled ? ", switched off in the ini" : "",
           info->present && !info->usable ? ", NOT usable" : "");
    if (!info->present) return;
    char path[4096];
    if (skyrim_plugin_child_path(directory, enb_effect_file(e), path, sizeof path) != 1) return;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size > ENB_FX_MAX_BYTES) { if (fd >= 0) close(fd); printf("    unreadable or too large\n"); return; }
    char *text = malloc((size_t)st.st_size + 1);
    EnbFx *fx = calloc(1, sizeof *fx);
    const ssize_t got = text ? read(fd, text, (size_t)st.st_size) : -1;
    close(fd);
    if (got == st.st_size && fx && !enb_fx_scan(text, (size_t)got, fx)) {
        printf("    techniques %u, passes %u, variables %u, images %u, uses 0x%x\n", fx->technique_count, info->pass_count,
               fx->uniform_count, fx->image_count, fx->uses);
        for (unsigned i = 0; i < fx->issue_count; ++i) {
            const EnbFxIssue *issue = &fx->issues[i];
            char found[4096];
            const char *note = "";
            if (issue->kind == ENB_FX_ISSUE_INCLUDE && skyrim_plugin_child_path(directory, issue->text, found, sizeof found) != 1) note = " (MISSING)";
            printf("    %s: %s%s\n", kinds[issue->kind], issue->text, note);
        }
        for (unsigned i = 0; i < fx->image_count; ++i) {
            if (!fx->images[i].resource[0]) continue;
            char found[4096];
            printf("    image resource %s%s\n", fx->images[i].resource,
                   skyrim_plugin_child_path(directory, fx->images[i].resource, found, sizeof found) == 1 ? "" : " (MISSING)");
        }
    } else {
        printf("    scan failed\n");
    }
    free(text);
    free(fx);
}

int main(int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        EnbPreset *preset = calloc(1, sizeof *preset);
        const int rc = preset ? enb_preset_load(argv[i], preset) : -1;
        printf("== %s\n", argv[i]);
        if (rc) { printf("  cannot load (error %d): no enbseries.ini at this folder\n", rc); status = 1; free(preset); continue; }
        char directory[4096];
        if (skyrim_plugin_child_path(argv[i], "enbseries", directory, sizeof directory) != 1) strcpy(directory, argv[i]);
        printf("  ini ok, native d3d9.dll %s (never loaded)\n", preset->has_native_dll ? "present" : "absent");
        for (int e = 0; e < ENB_EFFECT_COUNT; ++e) report_effect(directory, (EnbEffect)e, &preset->effects[e]);
        for (unsigned q = 0; q <= ENB_QUALITY_MAX; ++q) {
            EnbPlan plan;
            enb_plan_build(preset, q, 1, &plan);
            printf("  quality %u:", q);
            for (unsigned s = 0; s < plan.count; ++s) printf(" %s/%u(x%u)", names[plan.steps[s].effect], plan.steps[s].scale_div, plan.steps[s].pass_count);
            printf("%s cost %u\n", plan.needs_depth ? " [depth]" : "", plan.cost_units);
        }
        char line[256];
        enb_preset_diagnostic(preset, 3, line, sizeof line);
        printf("  %s\n", line);
        enb_preset_free(preset);
        free(preset);
    }
    return status;
}
