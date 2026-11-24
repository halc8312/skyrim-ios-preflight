/* SPDX-License-Identifier: GPL-3.0-only
 * ENB preset loading, pass planning, quality control and engine variables for
 * the ENB-compatible post-processing (docs/ENB_PLAN.md). Pure C: no rendering
 * and no game memory. The renderer (Codex) consumes EnbPlan and EnbVars.
 */
#pragma once
#include <stddef.h>
#include "enb_fx.h"
#include "enb_ini.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENB_EFFECT_ADAPTATION, ENB_EFFECT_BLOOM, ENB_EFFECT_LENS, ENB_EFFECT_PREPASS,
    ENB_EFFECT_DEPTH_OF_FIELD, ENB_EFFECT_MAIN, ENB_EFFECT_POSTPASS, ENB_EFFECT_COUNT
} EnbEffect;

typedef struct {
    int present;          /* the .fx file exists in enbseries/ */
    int enabled;          /* the ini switch (default on) */
    int usable;           /* scanned, no SM4 syntax, at least one technique */
    unsigned uses;        /* ENB_USES_* */
    unsigned technique_count, pass_count, unsupported_names, issue_count;
    int scan_status;      /* enb_fx_scan result */
} EnbEffectInfo;

typedef struct {
    EnbIni ini;
    EnbEffectInfo effects[ENB_EFFECT_COUNT];
    int has_ini, has_native_dll;  /* d3d9.dll next to the preset: never loaded */
} EnbPreset;

const char *enb_effect_file(EnbEffect);
/* Index of the last root that holds enbseries.ini (later roots win), or -1. */
int enb_preset_find(const char *const *roots, size_t count);
/* Read enbseries.ini and scan each effect file under root. 0, or an errno. Free with enb_preset_free. */
int enb_preset_load(const char *root, EnbPreset *out);
void enb_preset_free(EnbPreset *);

/* Quality 0..3 (docs/ENB_PLAN.md section 6). */
enum { ENB_QUALITY_MAX = 3, ENB_PLAN_MAX_STEPS = ENB_EFFECT_COUNT };
typedef struct {
    EnbEffect effect;
    unsigned scale_div;   /* working resolution is 1/scale_div of the game's */
    unsigned pass_count;
    int needs_depth;
} EnbPlanStep;
typedef struct {
    EnbPlanStep steps[ENB_PLAN_MAX_STEPS];
    unsigned count;
    int needs_depth;
    unsigned cost_units;  /* relative full-resolution fullscreen draws x100, for the governor's first guess */
} EnbPlan;
void enb_plan_build(const EnbPreset *, unsigned quality, int depth_available, EnbPlan *);

/* Adjust the quality from recent frame times. thermal: 0 nominal .. 3 critical. */
typedef struct { unsigned quality, max_quality, slow, fast, cooldown; } EnbGovernor;
void enb_governor_init(EnbGovernor *, unsigned max_quality, unsigned initial);
unsigned enb_governor_update(EnbGovernor *, float frame_ms, float target_ms, int thermal);

/* Engine-provided variables. Approximate until the game's time and weather are
 * read from the pinned image (Codex): hour of day 0..24, wall clock when unknown. */
typedef struct {
    float hour;              /* 0..24 */
    int interior;
    float seconds;           /* since the effect started */
    unsigned width, height;
    float fov_degrees;
} EnbGameState;
typedef struct {
    float timer[4], screen_size[4], tod1[4], tod2[4];
    float night_day, interior, field_of_view;
} EnbVars;
void enb_vars_compute(const EnbGameState *, EnbVars *);
/* Exponential adaptation toward a measured luminance. */
float enb_adapt_step(float previous, float measured, float dt_seconds, float time_constant);

/* Cheap result line for the diagnostic: numbers only. */
int enb_preset_diagnostic(const EnbPreset *, unsigned quality, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
