/* SPDX-License-Identifier: GPL-3.0-only
 * Structure scan of an ENB effect file (enbeffect.fx and friends) for the
 * ENB-compatible post-processing (docs/ENB_PLAN.md). It reads techniques,
 * passes, user-interface variables, images and the engine-provided names a
 * file uses. It does not compile HLSL (vkd3d-shader does that on the target)
 * and never executes anything. Comments are ignored; only DX9-style effects
 * (technique, sampler2D, SM1-3) are supported, and SM4 syntax is reported.
 */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ENB_FX_MAX_BYTES = 2 << 20, ENB_FX_MAX_TECHNIQUES = 16, ENB_FX_MAX_PASSES = 16,
    ENB_FX_MAX_UNIFORMS = 512, ENB_FX_MAX_IMAGES = 32, ENB_FX_MAX_INCLUDES = 16,
    ENB_FX_MAX_ISSUES = 32, ENB_FX_NAME = 64, ENB_FX_TEXT = 96
};

/* Engine-provided names a file may use. */
enum {
    ENB_USES_COLOR = 1u << 0, ENB_USES_ORIGINAL = 1u << 1, ENB_USES_BLOOM = 1u << 2, ENB_USES_LENS = 1u << 3,
    ENB_USES_ADAPTATION = 1u << 4, ENB_USES_DEPTH = 1u << 5, ENB_USES_MASK = 1u << 6, ENB_USES_NORMAL = 1u << 7,
    ENB_USES_PREVIOUS = 1u << 8, ENB_USES_ENB_VARIABLES = 1u << 9
};

typedef enum {
    ENB_FX_ISSUE_SM4_SYNTAX = 1,     /* technique11, Texture2D, SamplerState, .Sample( */
    ENB_FX_ISSUE_INCLUDE,            /* #include (resolved by the caller from the preset folder) */
    ENB_FX_ISSUE_UNSUPPORTED_NAME,   /* an engine image or variable the compatibility layer cannot supply */
    ENB_FX_ISSUE_LIMIT,              /* a table limit was reached */
    ENB_FX_ISSUE_NO_TECHNIQUE        /* no technique: nothing to run */
} EnbFxIssueKind;

typedef struct { EnbFxIssueKind kind; char text[ENB_FX_TEXT]; } EnbFxIssue;
typedef struct { char name[ENB_FX_NAME]; unsigned pass_count; char passes[ENB_FX_MAX_PASSES][ENB_FX_NAME]; } EnbFxTechnique;
typedef struct {
    char name[ENB_FX_NAME], type[16], ui_name[ENB_FX_NAME];
    float ui_min, ui_max;
    int has_range;
} EnbFxUniform;
typedef struct { char name[ENB_FX_NAME], resource[ENB_FX_NAME]; int is_sampler; } EnbFxImage;

typedef struct {
    EnbFxTechnique techniques[ENB_FX_MAX_TECHNIQUES];
    unsigned technique_count;
    EnbFxUniform uniforms[ENB_FX_MAX_UNIFORMS];
    unsigned uniform_count;
    EnbFxImage images[ENB_FX_MAX_IMAGES];
    unsigned image_count;
    char includes[ENB_FX_MAX_INCLUDES][ENB_FX_NAME];
    unsigned include_count;
    unsigned uses; /* ENB_USES_* */
    EnbFxIssue issues[ENB_FX_MAX_ISSUES];
    unsigned issue_count;
} EnbFx;

/* 0 on success (issues may still list unsupported things), EINVAL, EFBIG. */
int enb_fx_scan(const char *text, size_t length, EnbFx *out);
/* Lowering: plain HLSL for vkd3d-shader plus the effect-framework facts the
 * HLSL compiler does not take (techniques, pass shader entries, sampler states,
 * texture declarations). Annotations are removed from the HLSL text; includes
 * are left as #include for the caller to resolve first. */
enum { ENB_LOWER_MAX_SAMPLERS = 32, ENB_LOWER_MAX_TEXTURES = 32, ENB_LOWER_MAX_STATES = 16 };
typedef struct {
    char name[ENB_FX_NAME];
    char vs_entry[ENB_FX_NAME], vs_profile[16], ps_entry[ENB_FX_NAME], ps_profile[16];
    char render_target[ENB_FX_NAME]; /* the pass's RenderTarget state (a named intermediate image), or empty */
} EnbPassProgram;
typedef struct { char name[ENB_FX_NAME]; unsigned pass_count; EnbPassProgram passes[ENB_FX_MAX_PASSES]; } EnbTechniqueProgram;
typedef struct {
    char name[ENB_FX_NAME], texture[ENB_FX_NAME];
    char min_filter[16], mag_filter[16], mip_filter[16], address_u[16], address_v[16];
} EnbSamplerBinding;
typedef struct {
    char *hlsl;
    size_t hlsl_length;
    EnbTechniqueProgram techniques[ENB_FX_MAX_TECHNIQUES];
    unsigned technique_count;
    EnbSamplerBinding samplers[ENB_LOWER_MAX_SAMPLERS];
    unsigned sampler_count;
    char textures[ENB_LOWER_MAX_TEXTURES][ENB_FX_NAME], texture_resources[ENB_LOWER_MAX_TEXTURES][ENB_FX_NAME];
    unsigned texture_count;
    char ignored_states[ENB_LOWER_MAX_STATES][ENB_FX_NAME]; /* pass render states this layer does not apply */
    unsigned ignored_state_count;
} EnbLowered;
/* 0, EINVAL, EFBIG or ENOMEM. Free with enb_lowered_free. */
int enb_fx_lower(const char *text, size_t length, EnbLowered *out);
void enb_lowered_free(EnbLowered *);

/* How the compatibility layer treats an engine-provided name: 1 supplied by the
 * layer, 2 supplied only with the game's state (time, weather), 0 not supplied,
 * -1 not an ENB engine name. kind: 0 variable, 1 image. */
int enb_engine_name_status(const char *name, int *kind);
/* Parse "a, b, c" (up to 4 numbers) as written for vector variables; returns the count. */
int enb_value_parse(const char *text, float out[4]);

#ifdef __cplusplus
}
#endif
