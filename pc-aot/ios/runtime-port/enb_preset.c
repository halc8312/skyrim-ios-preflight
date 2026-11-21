/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "enb_preset.h"
#include "mod_plugins.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The ini switches live in [EFFECT]. The key names follow common presets and
 * are unverified; a missing key leaves the effect on when its file exists. */
static const struct { const char *file, *key; } effects[ENB_EFFECT_COUNT] = {
    [ENB_EFFECT_ADAPTATION] = {"enbadaptation.fx", "EnableAdaptation"},
    [ENB_EFFECT_BLOOM] = {"enbbloom.fx", "EnableBloom"},
    [ENB_EFFECT_LENS] = {"enblens.fx", "EnableLens"},
    [ENB_EFFECT_PREPASS] = {"enbeffectprepass.fx", "EnableEffectPrepass"},
    [ENB_EFFECT_DEPTH_OF_FIELD] = {"enbdepthoffield.fx", "EnableDepthOfField"},
    [ENB_EFFECT_MAIN] = {"enbeffect.fx", "EnableEffect"},
    [ENB_EFFECT_POSTPASS] = {"enbeffectpostpass.fx", "EnableEffectPostpass"},
};

const char *enb_effect_file(EnbEffect e) { return e < ENB_EFFECT_COUNT ? effects[e].file : NULL; }

/* A regular file named name in dir, matched without case; links are refused. */
static int regular_child(const char *dir, const char *name, char *out, size_t capacity) {
    struct stat st;
    return skyrim_plugin_child_path(dir, name, out, capacity) == 1 && !lstat(out, &st) && S_ISREG(st.st_mode);
}

int enb_preset_find(const char *const *roots, size_t count) {
    int found = -1;
    for (size_t i = 0; roots && i < count; ++i)
    {
        char path[4096];
        if (regular_child(roots[i], "enbseries.ini", path, sizeof path)) found = (int)i;
    }
    return found;
}

static int read_text(const char *path, char **text, size_t *length) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return EIO; }
    if (st.st_size > ENB_FX_MAX_BYTES) { close(fd); return EFBIG; }
    char *buffer = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (!buffer) { close(fd); return ENOMEM; }
    size_t have = 0;
    while (have < (size_t)st.st_size) {
        const ssize_t got = read(fd, buffer + have, (size_t)st.st_size - have);
        if (got <= 0) break;
        have += (size_t)got;
    }
    close(fd);
    if (have != (size_t)st.st_size) { free(buffer); return EIO; }
    *text = buffer;
    *length = have;
    return 0;
}

int enb_preset_load(const char *root, EnbPreset *out) {
    if (!root || !out) return EINVAL;
    *out = (EnbPreset){0};
    char path[4096];
    if (!regular_child(root, "enbseries.ini", path, sizeof path)) return ENOENT;
    int rc = enb_ini_read_file(path, &out->ini);
    if (rc) return rc;
    out->has_ini = 1;
    out->has_native_dll = regular_child(root, "d3d9.dll", path, sizeof path);
    char directory[4096];
    struct stat dir_stat;
    if (skyrim_plugin_child_path(root, "enbseries", directory, sizeof directory) != 1 || lstat(directory, &dir_stat) ||
        !S_ISDIR(dir_stat.st_mode)) return 0;
    for (int e = 0; e < ENB_EFFECT_COUNT; ++e) {
        EnbEffectInfo *info = &out->effects[e];
        if (!regular_child(directory, effects[e].file, path, sizeof path)) continue;
        info->present = 1;
        info->enabled = enb_ini_bool(&out->ini, "EFFECT", effects[e].key, 1);
        char *text = NULL;
        size_t length = 0;
        info->scan_status = read_text(path, &text, &length);
        if (info->scan_status) continue;
        EnbFx *fx = calloc(1, sizeof *fx);
        if (!fx) { free(text); return ENOMEM; }
        info->scan_status = enb_fx_scan(text, length, fx);
        free(text);
        if (!info->scan_status) {
            int sm4 = 0;
            info->uses = fx->uses;
            info->technique_count = fx->technique_count;
            info->issue_count = fx->issue_count;
            for (unsigned t = 0; t < fx->technique_count; ++t) info->pass_count += fx->techniques[t].pass_count;
            for (unsigned i = 0; i < fx->issue_count; ++i) {
                if (fx->issues[i].kind == ENB_FX_ISSUE_SM4_SYNTAX) sm4 = 1;
                if (fx->issues[i].kind == ENB_FX_ISSUE_UNSUPPORTED_NAME) ++info->unsupported_names;
            }
            info->usable = !sm4 && fx->technique_count > 0;
        }
        free(fx);
    }
    return 0;
}

void enb_preset_free(EnbPreset *preset) {
    if (preset) { enb_ini_free(&preset->ini); *preset = (EnbPreset){0}; }
}

/* Plan --------------------------------------------------------------------*/

static unsigned scale_for(EnbEffect e, unsigned quality) {
    switch (e) {
    case ENB_EFFECT_ADAPTATION: case ENB_EFFECT_BLOOM: case ENB_EFFECT_LENS:
        return quality <= 1 ? 4 : quality == 2 ? 2 : 1;
    case ENB_EFFECT_DEPTH_OF_FIELD: return quality <= 2 ? 2 : 1;
    default: return 1;
    }
}

void enb_plan_build(const EnbPreset *preset, unsigned quality, int depth_available, EnbPlan *out) {
    *out = (EnbPlan){0};
    if (!preset || !preset->has_ini) return;
    if (quality > ENB_QUALITY_MAX) quality = ENB_QUALITY_MAX;
    for (int e = 0; e < ENB_EFFECT_COUNT; ++e) {
        const EnbEffectInfo *info = &preset->effects[e];
        if (!info->present || !info->enabled || !info->usable) continue;
        /* Quality 0 keeps only the final composition. Depth of field needs
         * quality 2 and a depth image. */
        if (quality == 0 && e != ENB_EFFECT_MAIN) continue;
        const int depth = (info->uses & ENB_USES_DEPTH) != 0;
        if (depth && (!depth_available || quality < 2)) continue;
        if (e == ENB_EFFECT_DEPTH_OF_FIELD && quality < 2) continue;
        EnbPlanStep *step = &out->steps[out->count++];
        *step = (EnbPlanStep){.effect = (EnbEffect)e, .scale_div = scale_for((EnbEffect)e, quality),
                              .pass_count = info->pass_count ? info->pass_count : 1, .needs_depth = depth};
        out->needs_depth |= depth;
        out->cost_units += step->pass_count * 100u / (step->scale_div * step->scale_div);
    }
}

/* Governor ---------------------------------------------------------------*/

enum { SLOW_FRAMES = 30, FAST_FRAMES = 240, COOLDOWN_FRAMES = 120 };

void enb_governor_init(EnbGovernor *g, unsigned max_quality, unsigned initial) {
    if (max_quality > ENB_QUALITY_MAX) max_quality = ENB_QUALITY_MAX;
    *g = (EnbGovernor){.max_quality = max_quality, .quality = initial > max_quality ? max_quality : initial};
}

unsigned enb_governor_update(EnbGovernor *g, float frame_ms, float target_ms, int thermal) {
    if (!(target_ms > 0) || !isfinite(frame_ms) || frame_ms < 0) return g->quality;
    unsigned ceiling = g->max_quality;
    if (thermal >= 3) ceiling = 0;
    else if (thermal == 2 && ceiling > 1) ceiling = 1;
    if (g->quality > ceiling) { g->quality = ceiling; g->slow = g->fast = 0; g->cooldown = COOLDOWN_FRAMES; return g->quality; }
    if (g->cooldown) --g->cooldown;
    if (frame_ms > target_ms * 1.15f) { ++g->slow; g->fast = 0; }
    else if (frame_ms < target_ms * 0.8f) { ++g->fast; g->slow = 0; }
    else g->slow = g->fast = 0;
    if (g->slow >= SLOW_FRAMES && g->quality > 0 && !g->cooldown) {
        --g->quality; g->slow = g->fast = 0; g->cooldown = COOLDOWN_FRAMES;
    } else if (g->fast >= FAST_FRAMES && g->quality < ceiling && !g->cooldown) {
        ++g->quality; g->slow = g->fast = 0; g->cooldown = COOLDOWN_FRAMES;
    }
    return g->quality;
}

/* Variables --------------------------------------------------------------*/

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth(float edge0, float edge1, float x) {
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3 - 2 * t);
}

void enb_vars_compute(const EnbGameState *s, EnbVars *v) {
    *v = (EnbVars){0};
    float hour = fmodf(isfinite(s->hour) ? s->hour : 12.0f, 24.0f);
    if (hour < 0) hour += 24;
    /* Windows (hours): dawn 4-7, sunrise 6-9, day 8-17, sunset 17-20, dusk 19-22, night otherwise. */
    float part[5];  /* dawn, sunrise, day, sunset, dusk */
    part[2] = smooth(7, 9, hour) * (1 - smooth(17, 19, hour));
    part[0] = smooth(4, 5.5f, hour) * (1 - smooth(5.5f, 7, hour));
    part[1] = smooth(5.5f, 7, hour) * (1 - smooth(8, 9.5f, hour));
    part[3] = smooth(16.5f, 18, hour) * (1 - smooth(19, 20.5f, hour));
    part[4] = smooth(19, 20.5f, hour) * (1 - smooth(21.5f, 23, hour));
    float total = part[0] + part[1] + part[2] + part[3] + part[4];
    if (total > 1) { for (int i = 0; i < 5; ++i) part[i] /= total; total = 1; }   /* the weights always sum to one */
    v->tod1[0] = part[0]; v->tod1[1] = part[1]; v->tod1[2] = part[2]; v->tod1[3] = part[3];
    v->tod2[0] = part[4]; v->tod2[1] = 1 - total;
    v->night_day = clamp01(part[2] + 0.5f * (part[1] + part[3]));
    v->interior = s->interior ? 1.0f : 0.0f;
    v->field_of_view = s->fov_degrees;
    const float w = s->width ? (float)s->width : 1.0f, h = s->height ? (float)s->height : 1.0f;
    v->screen_size[0] = w; v->screen_size[1] = 1.0f / w; v->screen_size[2] = w / h; v->screen_size[3] = h / w;
    const float ms = s->seconds * 1000.0f;
    v->timer[0] = fmodf(ms, 16777216.0f) / 16777216.0f;
    v->timer[1] = s->seconds;
}

float enb_adapt_step(float previous, float measured, float dt, float tau) {
    if (!(dt > 0) || !(tau > 0) || !isfinite(measured) || !isfinite(previous)) return isfinite(measured) ? measured : previous;
    return measured + (previous - measured) * expf(-dt / tau);
}

int enb_preset_diagnostic(const EnbPreset *p, unsigned quality, char *out, size_t capacity) {
    unsigned present = 0, usable = 0, passes = 0, unsupported = 0;
    for (int e = 0; e < ENB_EFFECT_COUNT; ++e) {
        present += p->effects[e].present;
        const int used = p->effects[e].present && p->effects[e].enabled && p->effects[e].usable;
        usable += used;
        passes += used ? p->effects[e].pass_count : 0;
        unsupported += used ? p->effects[e].unsupported_names : 0;
    }
    return snprintf(out, capacity, "IOS_ENB schema=1 ini=%u native_dll=%u files=%u usable=%u passes=%u unsupported_names=%u quality=%u",
                    p->has_ini ? 1u : 0u, p->has_native_dll ? 1u : 0u, present, usable, passes, unsupported, quality);
}
