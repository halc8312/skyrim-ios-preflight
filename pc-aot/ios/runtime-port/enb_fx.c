/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "enb_fx.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Names and how the layer treats them. Unverified against real presets: the
 * list is meant to be corrected from the owner's files (plan section 5). */
static const struct { const char *name; int kind, status; unsigned uses; } engine[] = {
    {"Timer", 0, 1, ENB_USES_ENB_VARIABLES}, {"ScreenSize", 0, 1, ENB_USES_ENB_VARIABLES},
    {"AdaptiveQuality", 0, 1, ENB_USES_ENB_VARIABLES}, {"Weather", 0, 2, ENB_USES_ENB_VARIABLES},
    {"TimeOfDay1", 0, 2, ENB_USES_ENB_VARIABLES}, {"TimeOfDay2", 0, 2, ENB_USES_ENB_VARIABLES},
    {"ENightDayFactor", 0, 2, ENB_USES_ENB_VARIABLES}, {"EInteriorFactor", 0, 2, ENB_USES_ENB_VARIABLES},
    {"FieldOfView", 0, 2, ENB_USES_ENB_VARIABLES}, {"EBloomAmount", 0, 1, ENB_USES_ENB_VARIABLES},
    {"ENBParams01", 0, 0, ENB_USES_ENB_VARIABLES}, {"ENBParams02", 0, 0, ENB_USES_ENB_VARIABLES},
    {"LightParameters", 0, 0, ENB_USES_ENB_VARIABLES}, {"SunDirection", 0, 0, ENB_USES_ENB_VARIABLES},
    {"TextureColor", 1, 1, ENB_USES_COLOR}, {"TextureOriginal", 1, 1, ENB_USES_ORIGINAL},
    {"TextureBloom", 1, 1, ENB_USES_BLOOM}, {"TextureLens", 1, 1, ENB_USES_LENS},
    {"TextureAdaptation", 1, 1, ENB_USES_ADAPTATION}, {"TextureDepth", 1, 2, ENB_USES_DEPTH},
    {"TextureMask", 1, 0, ENB_USES_MASK}, {"TextureNormal", 1, 0, ENB_USES_NORMAL},
    {"TexturePrevious", 1, 0, ENB_USES_PREVIOUS},
};

int enb_engine_name_status(const char *name, int *kind) {
    for (size_t i = 0; name && i < sizeof engine / sizeof engine[0]; ++i)
        if (!strcmp(engine[i].name, name)) { if (kind) *kind = engine[i].kind; return engine[i].status; }
    return -1;
}

int enb_value_parse(const char *text, float out[4]) {
    int n = 0;
    while (text && *text && n < 4) {
        char *end;
        errno = 0;
        const double d = strtod(text, &end);
        if (end == text || errno || !isfinite(d)) return 0;
        out[n++] = (float)d;
        while (*end == ' ' || *end == '\t') ++end;
        if (*end == ',') ++end;
        else if (*end) return 0;
        text = end;
        while (*text == ' ' || *text == '\t') ++text;
    }
    return *text ? 0 : n;
}

typedef struct { const char *p, *end; } Cursor;
typedef struct { char text[ENB_FX_NAME]; int kind; } Token; /* 0 ident/number, 1 string, 2 punctuation, -1 end */

/* Skips whitespace and comments. */
static void skip_space(Cursor *c) {
    for (;;) {
        while (c->p < c->end && isspace((unsigned char)*c->p)) ++c->p;
        if (c->end - c->p >= 2 && c->p[0] == '/' && c->p[1] == '/') { while (c->p < c->end && *c->p != '\n') ++c->p; continue; }
        if (c->end - c->p >= 2 && c->p[0] == '/' && c->p[1] == '*') {
            c->p += 2;
            while (c->end - c->p >= 2 && !(c->p[0] == '*' && c->p[1] == '/')) ++c->p;
            c->p = c->end - c->p >= 2 ? c->p + 2 : c->end;
            continue;
        }
        return;
    }
}

static Token next(Cursor *c) {
    Token t = {"", -1};
    skip_space(c);
    if (c->p >= c->end) return t;
    const char ch = *c->p;
    if (isalnum((unsigned char)ch) || ch == '_' || ch == '.') {
        size_t n = 0;
        while (c->p < c->end && (isalnum((unsigned char)*c->p) || *c->p == '_' || *c->p == '.')) {
            if (n + 1 < sizeof t.text) t.text[n++] = *c->p;
            ++c->p;
        }
        t.text[n] = 0;
        t.kind = 0;
    } else if (ch == '"') {
        size_t n = 0;
        ++c->p;
        while (c->p < c->end && *c->p != '"' && *c->p != '\n') { if (n + 1 < sizeof t.text) t.text[n++] = *c->p; ++c->p; }
        if (c->p < c->end && *c->p == '"') ++c->p;
        t.text[n] = 0;
        t.kind = 1;
    } else {
        t.text[0] = ch;
        t.text[1] = 0;
        t.kind = 2;
        ++c->p;
    }
    return t;
}

static int is_punct(const Token *t, char ch) { return t->kind == 2 && t->text[0] == ch; }


static void issue(EnbFx *fx, EnbFxIssueKind kind, const char *text) {
    if (fx->issue_count == ENB_FX_MAX_ISSUES) return;
    for (unsigned i = 0; i < fx->issue_count; ++i)
        if (fx->issues[i].kind == kind && !strncmp(fx->issues[i].text, text, ENB_FX_TEXT - 1)) return;
    fx->issues[fx->issue_count].kind = kind;
    snprintf(fx->issues[fx->issue_count].text, ENB_FX_TEXT, "%s", text);
    ++fx->issue_count;
}

static void limit(EnbFx *fx, const char *what) { issue(fx, ENB_FX_ISSUE_LIMIT, what); }
static void note_engine_name(EnbFx *fx, const char *name);

static void skip_block(Cursor *c, EnbFx *fx) { /* after the opening brace; function bodies use engine names too */
    int depth = 1;
    while (depth > 0) {
        Token t = next(c);
        if (t.kind < 0) return;
        if (is_punct(&t, '{')) ++depth;
        else if (is_punct(&t, '}')) --depth;
        else if (t.kind == 0) {
            note_engine_name(fx, t.text);
        }
    }
}

typedef struct { char name[ENB_FX_NAME], type[ENB_FX_NAME], text[ENB_FX_NAME]; int has_text; float number; int has_number; } Annotation;

/* After '<': read "type Name = value;" entries up to '>'. */
static void read_annotations(Cursor *c, Annotation *list, unsigned *count, unsigned capacity) {
    *count = 0;
    for (;;) {
        Token type = next(c);
        if (type.kind < 0 || is_punct(&type, '>')) return;
        Token name = next(c);
        if (name.kind != 0) continue;
        Token eq = next(c);
        if (!is_punct(&eq, '=')) continue;
        Token value = next(c);
        float sign = 1;
        if (is_punct(&value, '-')) { sign = -1; value = next(c); }
        if (*count < capacity) {
            Annotation *a = &list[(*count)++];
            *a = (Annotation){0};
            snprintf(a->name, sizeof a->name, "%s", name.text);
            snprintf(a->type, sizeof a->type, "%s", type.text);
            if (value.kind == 1) { snprintf(a->text, sizeof a->text, "%s", value.text); a->has_text = 1; }
            else { char *end; a->number = sign * (float)strtod(value.text, &end); a->has_number = end != value.text; }
        }
        Token semi = next(c);
        if (is_punct(&semi, '>')) return;
    }
}

static int type_is_scalar(const char *t) {
    static const char *const types[] = {"float", "float2", "float3", "float4", "float4x4", "float3x3", "float2x2", "int", "bool", "half", "half2", "half3", "half4"};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; ++i) if (!strcasecmp(t, types[i])) return 1;
    return 0;
}

static int type_is_image(const char *t, int *sampler, int *sm4) {
    *sampler = *sm4 = 0;
    if (!strcasecmp(t, "texture") || !strcasecmp(t, "texture2d") || !strcasecmp(t, "texture3d") || !strcasecmp(t, "texturecube")) {
        *sm4 = !strcmp(t, "Texture2D") || !strcmp(t, "Texture3D");
        return 1;
    }
    if (!strncasecmp(t, "sampler", 7)) { *sampler = 1; *sm4 = !strcmp(t, "SamplerState"); return 1; }
    return 0;
}

static void note_engine_name(EnbFx *fx, const char *full) {
    int kind;
    char name[ENB_FX_NAME];
    snprintf(name, sizeof name, "%s", full);
    char *member = strchr(name, '.');   /* Timer.x, tex.Sample */
    if (member) {
        if (!strncmp(member, ".Sample", 7)) issue(fx, ENB_FX_ISSUE_SM4_SYNTAX, ".Sample");
        *member = 0;
    }
    const int status = enb_engine_name_status(name, &kind);
    if (status < 0) return;
    for (size_t i = 0; i < sizeof engine / sizeof engine[0]; ++i)
        if (!strcmp(engine[i].name, name)) fx->uses |= engine[i].uses;
    if (status == 0) issue(fx, ENB_FX_ISSUE_UNSUPPORTED_NAME, name);
}

static void add_technique(EnbFx *fx, Cursor *c, Token name) {
    EnbFxTechnique *t = fx->technique_count < ENB_FX_MAX_TECHNIQUES ? &fx->techniques[fx->technique_count++] : NULL;
    if (!t) limit(fx, "techniques");
    if (t) snprintf(t->name, sizeof t->name, "%s", name.kind == 0 ? name.text : "");
    Token tok = name;
    while (tok.kind >= 0 && !is_punct(&tok, '{')) {
        if (is_punct(&tok, '<')) { Annotation skip[8]; unsigned n; read_annotations(c, skip, &n, 8); }
        tok = next(c);
    }
    int depth = 1;
    while (depth > 0) {
        tok = next(c);
        if (tok.kind < 0) return;
        if (is_punct(&tok, '{')) ++depth;
        else if (is_punct(&tok, '}')) --depth;
        else if (depth == 1 && tok.kind == 0 && !strcmp(tok.text, "pass")) {
            Token pass = next(c);
            if (t && t->pass_count < ENB_FX_MAX_PASSES) snprintf(t->passes[t->pass_count++], ENB_FX_NAME, "%s", pass.kind == 0 ? pass.text : "");
            else if (t) limit(fx, "passes");
        } else if (tok.kind == 0) {
            note_engine_name(fx, tok.text);
        }
    }
}

int enb_fx_scan(const char *text, size_t length, EnbFx *out) {
    if (!out || (length && !text)) return EINVAL;
    *out = (EnbFx){0};
    if (length > ENB_FX_MAX_BYTES) return EFBIG;
    Cursor c = {text, text + length};
    /* #include lines are not tokens: find them first, on whole lines outside comments. */
    for (const char *line = text; line < text + length;) {
        const char *stop = memchr(line, '\n', (size_t)(text + length - line));
        const char *end = stop ? stop : text + length;
        const char *p = line;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (end - p > 8 && !strncmp(p, "#include", 8)) {
            const char *q = memchr(p, '"', (size_t)(end - p));
            const char *r = q ? memchr(q + 1, '"', (size_t)(end - q - 1)) : NULL;
            if (q && r) {
                if (out->include_count < ENB_FX_MAX_INCLUDES) {
                    snprintf(out->includes[out->include_count++], ENB_FX_NAME, "%.*s", (int)(r - q - 1), q + 1);
                } else limit(out, "includes");
            }
        }
        line = stop ? stop + 1 : text + length;
    }
    for (unsigned i = 0; i < out->include_count; ++i) issue(out, ENB_FX_ISSUE_INCLUDE, out->includes[i]);
    for (;;) {
        Token first = next(&c);
        if (first.kind < 0) break;
        if (first.kind == 2) {
            if (is_punct(&first, '#')) { /* skip the directive line */
                while (c.p < c.end && *c.p != '\n') ++c.p;
            } else if (is_punct(&first, '{')) skip_block(&c, out);
            continue;
        }
        if (first.kind != 0) continue;
        if (!strncasecmp(first.text, "technique", 9)) {
            if (!strcmp(first.text, "technique10") || !strcmp(first.text, "technique11"))
                issue(out, ENB_FX_ISSUE_SM4_SYNTAX, first.text);
            add_technique(out, &c, next(&c));
            continue;
        }
        /* A declaration, function or struct: collect up to ';' or a block. */
        Token stmt[24];
        unsigned count = 0;
        int has_paren = 0, has_assign = 0;
        Annotation notes[12];
        unsigned note_count = 0;
        Token tok = first;
        int body = 0;
        for (;;) {
            if (tok.kind < 0) break;
            if (is_punct(&tok, ';')) break;
            if (is_punct(&tok, '{')) { body = 1; break; }
            if (is_punct(&tok, '<')) read_annotations(&c, notes, &note_count, 12);
            else {
                if (is_punct(&tok, '(')) has_paren = 1;
                if (is_punct(&tok, '=')) has_assign = 1;
                if (count < 24) stmt[count++] = tok;
                if (tok.kind == 0) note_engine_name(out, tok.text);
            }
            tok = next(&c);
        }
        if (body) {
            skip_block(&c, out);
            if (!has_paren) { /* struct or sampler_state initializer: continue to ';' */
                for (;;) { Token t = next(&c); if (t.kind < 0 || is_punct(&t, ';')) break; }
            }
            if (has_paren) continue;
        }
        if (has_paren || !count) continue;
        /* modifiers, type, name */
        unsigned at = 0;
        int is_static = 0;
        while (at < count && stmt[at].kind == 0 && (!strcmp(stmt[at].text, "uniform") || !strcmp(stmt[at].text, "static") ||
               !strcmp(stmt[at].text, "const") || !strcmp(stmt[at].text, "extern") || !strcmp(stmt[at].text, "shared") ||
               !strcmp(stmt[at].text, "volatile"))) { if (!strcmp(stmt[at].text, "static") || !strcmp(stmt[at].text, "const")) is_static = 1; ++at; }
        (void)has_assign;
        if (at + 1 >= count || stmt[at].kind != 0 || stmt[at + 1].kind != 0) continue;
        const char *type = stmt[at].text, *name = stmt[at + 1].text;
        int sampler, sm4;
        if (type_is_image(type, &sampler, &sm4)) {
            if (sm4) issue(out, ENB_FX_ISSUE_SM4_SYNTAX, type);
            if (out->image_count == ENB_FX_MAX_IMAGES) { limit(out, "images"); continue; }
            EnbFxImage *image = &out->images[out->image_count++];
            snprintf(image->name, sizeof image->name, "%s", name);
            image->is_sampler = sampler;
            for (unsigned i = 0; i < note_count; ++i)
                if (notes[i].has_text && !strcasecmp(notes[i].name, "ResourceName")) snprintf(image->resource, sizeof image->resource, "%s", notes[i].text);
        } else if (type_is_scalar(type) && !is_static) {
            if (out->uniform_count == ENB_FX_MAX_UNIFORMS) { limit(out, "uniforms"); continue; }
            EnbFxUniform *u = &out->uniforms[out->uniform_count++];
            snprintf(u->name, sizeof u->name, "%s", name);
            snprintf(u->type, sizeof u->type, "%.15s", type);
            float lo = 0, hi = 0;
            int have_lo = 0, have_hi = 0;
            for (unsigned i = 0; i < note_count; ++i) {
                if (notes[i].has_text && !strcasecmp(notes[i].name, "UIName")) snprintf(u->ui_name, sizeof u->ui_name, "%s", notes[i].text);
                if (notes[i].has_number && !strcasecmp(notes[i].name, "UIMin")) { lo = notes[i].number; have_lo = 1; }
                if (notes[i].has_number && !strcasecmp(notes[i].name, "UIMax")) { hi = notes[i].number; have_hi = 1; }
            }
            if (have_lo && have_hi && lo <= hi) { u->ui_min = lo; u->ui_max = hi; u->has_range = 1; }
        }
    }
    if (!out->technique_count) issue(out, ENB_FX_ISSUE_NO_TECHNIQUE, "");
    return 0;
}
