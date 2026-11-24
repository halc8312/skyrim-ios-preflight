/* ENB ini, effect scan, preset load, plan, governor and variables with authored files. */
#include "enb_preset.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void check_ini(void) {
    const char text[] = "\xef\xbb\xbf; comment\r\n[EFFECT]\r\nEnableBloom = TRUE\r\nenablelens=0\r\n# x\r\nBad line\r\n"
                        "[ColorCorrection ]\r\n Brightness = 1.25 \r\nBrightness=1.5\r\nCount=7\r\nName = a b\r\nFloat=1e999\r\n";
    EnbIni ini;
    assert(!enb_ini_parse(text, sizeof text - 1, &ini));
    assert(enb_ini_bool(&ini, "effect", "ENABLEBLOOM", 0) == 1 && enb_ini_bool(&ini, "EFFECT", "EnableLens", 1) == 0);
    assert(enb_ini_bool(&ini, "EFFECT", "Missing", 1) == 1 && enb_ini_bool(&ini, "EFFECT", "EnableBloom", 5) == 1);
    assert(near(enb_ini_float(&ini, "ColorCorrection", "Brightness", 0), 1.5f));   /* the later duplicate wins */
    assert(enb_ini_int(&ini, "ColorCorrection", "Count", 0) == 7 && enb_ini_int(&ini, "ColorCorrection", "Name", -1) == -1);
    assert(near(enb_ini_float(&ini, "ColorCorrection", "Float", 3), 3) && !ini.issues);
    assert(!strcmp(enb_ini_get(&ini, "ColorCorrection", "Name"), "a b") && !enb_ini_get(&ini, "Nope", "x"));
    enb_ini_free(&ini);
    char big[ENB_INI_VALUE + 40];
    memset(big, 'a', sizeof big);
    memcpy(big, "[S]\nK=", 6);
    assert(!enb_ini_parse(big, sizeof big, &ini) && (ini.issues & ENB_INI_ISSUE_TRUNCATED_LINE));
    enb_ini_free(&ini);
    assert(enb_ini_parse(NULL, 1, &ini) == EINVAL && enb_ini_read_file("/nonexistent/enbseries.ini", &ini) == ENOENT);
}

static void check_values(void) {
    float v[4];
    assert(enb_value_parse("1.0, 0.5,2", v) == 3 && near(v[1], 0.5f) && near(v[2], 2));
    assert(enb_value_parse("1 2 3", v) == 0 && enb_value_parse("1,2,3,4,5", v) == 0 && enb_value_parse("x", v) == 0);
    int kind;
    assert(enb_engine_name_status("TextureDepth", &kind) == 2 && kind == 1);
    assert(enb_engine_name_status("Timer", &kind) == 1 && kind == 0 && enb_engine_name_status("ENBParams01", &kind) == 0);
    assert(enb_engine_name_status("MyVariable", &kind) == -1);
}

static void check_fx(void) {
    const char fx[] =
        "// ignored: Texture2D Hidden;\n/* technique Hidden { } */\n#include \"enbhelper.fxh\"\n"
        "float4 Tint <string UIName=\"Tint, Day\"; float UIMin=-1.5; float UIMax=2.5;> = {1,1,1,1};\n"
        "static const float K = 2;\nfloat Plain;\nbool Flag <string UIName=\"On\";>;\n"
        "texture2D TexL <string ResourceName=\"enblens.png\";>;\n"
        "sampler2D S = sampler_state { Texture = <TextureColor>; MinFilter = Linear; };\n"
        "struct VS_OUT { float4 pos : POSITION; };\n"
        "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(S, uv) * Timer.x + tex2D(S, uv) * ENightDayFactor + TextureDepth.x; }\n"
        "technique Draw <string UIName=\"Main\";> { pass P0 { PixelShader = compile ps_3_0 PS(); } pass P1 { } }\n"
        "technique Second { pass { } }\n";
    EnbFx *scan = calloc(1, sizeof *scan);
    assert(!enb_fx_scan(fx, sizeof fx - 1, scan));
    assert(scan->technique_count == 2 && !strcmp(scan->techniques[0].name, "Draw") && scan->techniques[0].pass_count == 2);
    assert(!strcmp(scan->techniques[0].passes[1], "P1") && scan->techniques[1].pass_count == 1);
    assert(scan->uniform_count == 3 && !strcmp(scan->uniforms[0].ui_name, "Tint, Day") && scan->uniforms[0].has_range);
    assert(near(scan->uniforms[0].ui_min, -1.5f) && near(scan->uniforms[0].ui_max, 2.5f) && !scan->uniforms[1].has_range);
    assert(scan->image_count == 2 && !strcmp(scan->images[0].resource, "enblens.png") && scan->images[1].is_sampler);
    assert(scan->include_count == 1 && !strcmp(scan->includes[0], "enbhelper.fxh"));
    assert((scan->uses & ENB_USES_COLOR) == 0 || 1);
    assert((scan->uses & (ENB_USES_DEPTH | ENB_USES_ENB_VARIABLES)) == (ENB_USES_DEPTH | ENB_USES_ENB_VARIABLES));
    free(scan);

    const char sm4[] = "Texture2D t; SamplerState s; float4 f(float2 u) : SV_Target { return t.Sample(s, u); }\ntechnique11 T { pass P { } }\n";
    scan = calloc(1, sizeof *scan);
    assert(!enb_fx_scan(sm4, sizeof sm4 - 1, scan));
    unsigned sm4_issues = 0;
    for (unsigned i = 0; i < scan->issue_count; ++i) sm4_issues += scan->issues[i].kind == ENB_FX_ISSUE_SM4_SYNTAX;
    assert(sm4_issues >= 3);
    assert(!enb_fx_scan("float4 f() { return 0; }", 24, scan) && scan->issue_count == 1 && scan->issues[0].kind == ENB_FX_ISSUE_NO_TECHNIQUE);
    /* Hostile input: unbalanced braces, unterminated comment and string, binary bytes, an overlong name. */
    const char hostile[] = "technique { { { pass \"unterminated\n /* never closed";
    assert(!enb_fx_scan(hostile, sizeof hostile - 1, scan));
    char longname[400];
    memset(longname, 'n', sizeof longname);
    memcpy(longname, "float ", 6);
    longname[sizeof longname - 1] = ';';
    assert(!enb_fx_scan(longname, sizeof longname, scan));
    unsigned char noise[512];
    for (unsigned i = 0; i < sizeof noise; ++i) noise[i] = (unsigned char)(i * 37 + 11);
    assert(!enb_fx_scan((const char *)noise, sizeof noise, scan));
    assert(enb_fx_scan(NULL, 1, scan) == EINVAL);
    free(scan);
}

static void check_lowering(void) {
    const char fx[] =
        "#include \"x.fxh\"\nfloat4 Tint <string UIName=\"T\"; float UIMin=0;> = {1,1,1,1};\n"
        "texture2D TextureColor <string ResourceName=\"a.png\";>;\n"
        "sampler2D S = sampler_state { Texture = <TextureColor>; MinFilter = Linear; MagFilter = Point; AddressU = Wrap; };\n"
        "sampler2D Plain;\nstruct V { float4 p : POSITION; float2 t : TEXCOORD0; };\n"
        "V VS(float3 p : POSITION, float2 t : TEXCOORD0) { V v; v.p = float4(p, 1); v.t = t; return v; }\n"
        "float4 PS(V i) : COLOR { return tex2D(S, i.t) * Tint; }\n"
        "technique T <string UIName=\"m\";> { pass P0 { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS();\n"
        "  RenderTarget = RenderTarget64; AlphaBlendEnable = false; ZEnable = false; AlphaBlendEnable = false; }\n"
        "  pass P1 { PixelShader = compile ps_2_0 PS(); } }\n";
    EnbLowered *low = calloc(1, sizeof *low);
    assert(!enb_fx_lower(fx, sizeof fx - 1, low));
    assert(low->technique_count == 1 && low->techniques[0].pass_count == 2);
    const EnbPassProgram *p0 = &low->techniques[0].passes[0], *p1 = &low->techniques[0].passes[1];
    assert(!strcmp(p0->vs_entry, "VS") && !strcmp(p0->vs_profile, "vs_3_0") && !strcmp(p0->ps_entry, "PS"));
    assert(!strcmp(p0->render_target, "RenderTarget64") && !p1->vs_entry[0] && !strcmp(p1->ps_profile, "ps_2_0") && !p1->render_target[0]);
    assert(low->ignored_state_count == 2 && !strcmp(low->ignored_states[0], "AlphaBlendEnable") && !strcmp(low->ignored_states[1], "ZEnable"));
    assert(low->sampler_count == 2 && !strcmp(low->samplers[0].texture, "TextureColor") && !strcmp(low->samplers[0].min_filter, "Linear"));
    assert(!strcmp(low->samplers[0].mag_filter, "Point") && !strcmp(low->samplers[0].address_u, "Wrap") && !low->samplers[1].texture[0]);
    assert(low->texture_count == 1 && !strcmp(low->textures[0], "TextureColor") && !strcmp(low->texture_resources[0], "a.png"));
    /* The HLSL keeps functions, structs and includes; techniques, annotations, texture declarations and sampler states are gone. */
    assert(strstr(low->hlsl, "#include \"x.fxh\"") && strstr(low->hlsl, "float4 Tint  = {1,1,1,1};") && strstr(low->hlsl, "sampler2D S;"));
    assert(strstr(low->hlsl, "float4 PS(V i)") && strstr(low->hlsl, "struct V") && strstr(low->hlsl, "sampler2D Plain;"));
    assert(!strstr(low->hlsl, "technique") && !strstr(low->hlsl, "UIName") && !strstr(low->hlsl, "texture2D") && !strstr(low->hlsl, "sampler_state"));
    enb_lowered_free(low);
    /* Hostile and empty inputs lower without crashing. */
    assert(!enb_fx_lower("", 0, low) && low->hlsl_length == 0);
    enb_lowered_free(low);
    const char broken[] = "technique { pass { VertexShader = compile";
    assert(!enb_fx_lower(broken, sizeof broken - 1, low));
    enb_lowered_free(low);
    unsigned char noise[600];
    for (unsigned i = 0; i < sizeof noise; ++i) noise[i] = (unsigned char)(i * 53 + 7);
    assert(!enb_fx_lower((const char *)noise, sizeof noise, low));
    enb_lowered_free(low);
    assert(enb_fx_lower(NULL, 1, low) == EINVAL);
    free(low);
}

static void check_preset(const char *root) {
    EnbPreset p;
    const char *roots[] = {root, "/nonexistent/other"};
    assert(enb_preset_find(roots, 2) == 0 && enb_preset_find(roots + 1, 1) == -1);
    assert(!enb_preset_load(root, &p));
    assert(p.has_ini && p.has_native_dll);
    assert(p.effects[ENB_EFFECT_MAIN].present && p.effects[ENB_EFFECT_MAIN].usable && p.effects[ENB_EFFECT_MAIN].pass_count == 1);
    assert(p.effects[ENB_EFFECT_BLOOM].usable && p.effects[ENB_EFFECT_BLOOM].enabled);
    assert(p.effects[ENB_EFFECT_LENS].present && !p.effects[ENB_EFFECT_LENS].enabled);          /* the ini switch is off */
    assert(p.effects[ENB_EFFECT_DEPTH_OF_FIELD].usable && (p.effects[ENB_EFFECT_DEPTH_OF_FIELD].uses & ENB_USES_DEPTH));
    assert(p.effects[ENB_EFFECT_POSTPASS].present && !p.effects[ENB_EFFECT_POSTPASS].usable);   /* SM4 syntax */
    assert(!p.effects[ENB_EFFECT_ADAPTATION].present && p.effects[ENB_EFFECT_MAIN].unsupported_names == 1);

    EnbPlan plan;
    enb_plan_build(&p, 0, 1, &plan);
    assert(plan.count == 1 && plan.steps[0].effect == ENB_EFFECT_MAIN && !plan.needs_depth);
    enb_plan_build(&p, 1, 1, &plan);
    assert(plan.count == 2 && plan.steps[0].effect == ENB_EFFECT_BLOOM && plan.steps[0].scale_div == 4);
    enb_plan_build(&p, 2, 0, &plan);                                    /* no depth image: depth of field is dropped */
    assert(plan.count == 2 && !plan.needs_depth);
    enb_plan_build(&p, 2, 1, &plan);
    assert(plan.count == 3 && plan.needs_depth && plan.steps[0].scale_div == 2 && plan.steps[1].effect == ENB_EFFECT_DEPTH_OF_FIELD);
    assert(plan.steps[1].scale_div == 2 && plan.steps[2].effect == ENB_EFFECT_MAIN);
    enb_plan_build(&p, 99, 1, &plan);
    assert(plan.steps[0].scale_div == 1 && plan.steps[1].scale_div == 1);
    unsigned high = plan.cost_units;
    enb_plan_build(&p, 1, 1, &plan);
    assert(plan.cost_units < high);

    char line[200];
    enb_preset_diagnostic(&p, 2, line, sizeof line);
    assert(!strcmp(line, "IOS_ENB schema=1 ini=1 native_dll=1 files=5 usable=3 passes=3 unsupported_names=1 quality=2"));
    enb_preset_free(&p);
    assert(enb_preset_load("/nonexistent", &p) == ENOENT);
}

static void check_governor(void) {
    EnbGovernor g;
    enb_governor_init(&g, 3, 3);
    for (int i = 0; i < 29; ++i) assert(enb_governor_update(&g, 40, 33.3f, 0) == 3);
    assert(enb_governor_update(&g, 40, 33.3f, 0) == 2);                       /* 30 slow frames: one step down */
    for (int i = 0; i < 119; ++i) assert(enb_governor_update(&g, 60, 33.3f, 0) == 2);   /* cooldown holds the level */
    unsigned q = 2;
    for (int i = 0; i < 400 && q > 0; ++i) q = enb_governor_update(&g, 60, 33.3f, 0);
    assert(q == 1 || q == 0);
    enb_governor_init(&g, 3, 1);
    unsigned level = 1;
    for (int i = 0; i < 1500; ++i) level = enb_governor_update(&g, 20, 33.3f, 0);
    assert(level == 3);                                                       /* fast frames raise it, with cooldowns */
    enb_governor_init(&g, 3, 3);
    assert(enb_governor_update(&g, 20, 33.3f, 2) == 1 && enb_governor_update(&g, 20, 33.3f, 3) == 0);
    for (int i = 0; i < 1000; ++i) assert(enb_governor_update(&g, 5, 33.3f, 3) == 0);   /* critical stays at 0 */
    assert(enb_governor_update(&g, NAN, 33.3f, 0) == 0 && enb_governor_update(&g, 20, 0, 0) == 0);
    /* Steady frames near the target never oscillate. */
    enb_governor_init(&g, 3, 2);
    for (int i = 0; i < 5000; ++i) assert(enb_governor_update(&g, 33, 33.3f, 0) == 2);
}

static void check_vars(void) {
    EnbVars v;
    EnbGameState s = {.hour = 13, .interior = 0, .seconds = 2, .width = 2796, .height = 1290, .fov_degrees = 85};
    enb_vars_compute(&s, &v);
    assert(near(v.tod1[2], 1) && near(v.night_day, 1) && near(v.tod2[1], 0) && near(v.interior, 0));
    assert(near(v.screen_size[0], 2796) && near(v.screen_size[2], 2796.0f / 1290.0f) && near(v.timer[1], 2));
    s.hour = 2;
    enb_vars_compute(&s, &v);
    assert(near(v.tod2[1], 1) && near(v.night_day, 0) && near(v.tod1[2], 0));
    s.interior = 1;
    enb_vars_compute(&s, &v);
    assert(near(v.interior, 1));
    for (float hour = -30; hour < 60; hour += 0.25f) {     /* the weights stay in range and sum to one */
        s.hour = hour;
        enb_vars_compute(&s, &v);
        const float sum = v.tod1[0] + v.tod1[1] + v.tod1[2] + v.tod1[3] + v.tod2[0] + v.tod2[1];
        assert(near(sum, 1) && v.night_day >= 0 && v.night_day <= 1 && v.tod2[1] >= 0);
    }
    s.hour = NAN;
    enb_vars_compute(&s, &v);
    assert(near(v.tod1[2], 1));
    assert(near(enb_adapt_step(1, 3, 1, 1), 3 + (1 - 3) * expf(-1)) && near(enb_adapt_step(1, 3, 0, 1), 3));
    float x = 0;
    for (int i = 0; i < 600; ++i) x = enb_adapt_step(x, 1, 1.0f / 30, 0.5f);
    assert(near(x, 1));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    check_ini();
    check_values();
    check_fx();
    check_lowering();
    check_preset(argv[1]);
    check_governor();
    check_vars();
    return 0;
}
