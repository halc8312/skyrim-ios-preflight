/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_racemenu_preset.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_DEPTH = 24, MAX_NODES = 16384 };
typedef enum { T_NULL, T_BOOL, T_NUMBER, T_STRING, T_ARRAY, T_OBJECT } Type;

typedef struct {
    Type type;
    const char *text; /* string contents (raw, still escaped) */
    size_t length;
    double number;
    int first, next;  /* child and sibling node indexes, -1 for none */
    const char *key;  /* raw key text in the parent object */
    size_t key_length;
    unsigned count;   /* children */
} Node;

typedef struct {
    const char *p, *end;
    Node *nodes;
    unsigned used;
    int failed;
} Parser;

static void skip(Parser *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) ++s->p;
}

static int new_node(Parser *s, Type type) {
    if (s->used >= MAX_NODES) { s->failed = 1; return -1; }
    Node *n = &s->nodes[s->used];
    *n = (Node){.type = type, .first = -1, .next = -1};
    return (int)s->used++;
}

/* Raw string body between quotes; escapes are validated, not decoded. */
static int read_string(Parser *s, const char **text, size_t *length) {
    if (s->p >= s->end || *s->p != '"') return 0;
    const char *start = ++s->p;
    while (s->p < s->end && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p;
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (++s->p >= s->end) return 0;
            char e = *s->p;
            if (e == 'u') {
                for (int i = 1; i <= 4; ++i)
                    if (s->p + i >= s->end || !strchr("0123456789abcdefABCDEF", s->p[i]) || !s->p[i]) return 0;
                s->p += 4;
            } else if (!e || !strchr("\"\\/bfnrt", e)) return 0;
        }
        ++s->p;
    }
    if (s->p >= s->end) return 0;
    *text = start;
    *length = (size_t)(s->p - start);
    ++s->p;
    return 1;
}

static int parse_value(Parser *s, unsigned depth);

static int parse_number(Parser *s) {
    const char *q = s->p;
    if (q < s->end && *q == '-') ++q;
    if (q >= s->end) return -1;
    if (*q == '0') ++q;
    else if (*q >= '1' && *q <= '9') while (q < s->end && *q >= '0' && *q <= '9') ++q;
    else return -1;
    if (q < s->end && *q == '.') {
        const char *d = ++q;
        while (q < s->end && *q >= '0' && *q <= '9') ++q;
        if (q == d) return -1;
    }
    if (q < s->end && (*q == 'e' || *q == 'E')) {
        ++q;
        if (q < s->end && (*q == '+' || *q == '-')) ++q;
        const char *d = q;
        while (q < s->end && *q >= '0' && *q <= '9') ++q;
        if (q == d) return -1;
    }
    char token[64];
    size_t n = (size_t)(q - s->p);
    if (n >= sizeof token) return -1;
    memcpy(token, s->p, n);
    token[n] = 0;
    int id = new_node(s, T_NUMBER);
    if (id < 0) return -1;
    errno = 0;
    s->nodes[id].number = strtod(token, NULL);
    s->p = q;
    return id;
}

static int parse_value(Parser *s, unsigned depth) {
    skip(s);
    if (s->p >= s->end || depth > MAX_DEPTH) return -1;
    char c = *s->p;
    if (c == '"') {
        int id = new_node(s, T_STRING);
        if (id < 0 || !read_string(s, &s->nodes[id].text, &s->nodes[id].length)) return -1;
        return id;
    }
    if (c == '{' || c == '[') {
        const int object = c == '{';
        int id = new_node(s, object ? T_OBJECT : T_ARRAY);
        if (id < 0) return -1;
        ++s->p;
        skip(s);
        if (s->p < s->end && *s->p == (object ? '}' : ']')) { ++s->p; return id; }
        int last = -1;
        for (;;) {
            const char *key = NULL;
            size_t key_length = 0;
            skip(s);
            if (object) {
                if (!read_string(s, &key, &key_length)) return -1;
                skip(s);
                if (s->p >= s->end || *s->p++ != ':') return -1;
            }
            int child = parse_value(s, depth + 1);
            if (child < 0) return -1;
            s->nodes[child].key = key;
            s->nodes[child].key_length = key_length;
            if (last < 0) s->nodes[id].first = child; else s->nodes[last].next = child;
            last = child;
            ++s->nodes[id].count;
            skip(s);
            if (s->p >= s->end) return -1;
            if (*s->p == ',') { ++s->p; continue; }
            if (*s->p++ == (object ? '}' : ']')) return id;
            return -1;
        }
    }
    if (s->end - s->p >= 4 && !strncmp(s->p, "true", 4)) { s->p += 4; return new_node(s, T_BOOL); }
    if (s->end - s->p >= 5 && !strncmp(s->p, "false", 5)) { s->p += 5; return new_node(s, T_BOOL); }
    if (s->end - s->p >= 4 && !strncmp(s->p, "null", 4)) { s->p += 4; return new_node(s, T_NULL); }
    return parse_number(s);
}

/* Child with this exact key. *duplicate is set when the key appears twice. */
static int member(const Node *nodes, int object, const char *key, int *duplicate) {
    int found = -1;
    size_t n = strlen(key);
    if (nodes[object].type != T_OBJECT) return -1;
    for (int c = nodes[object].first; c >= 0; c = nodes[c].next)
        if (nodes[c].key_length == n && !memcmp(nodes[c].key, key, n)) {
            if (found >= 0) *duplicate = 1;
            found = c;
        }
    return found;
}

static unsigned non_empty(const Node *nodes, int object, const char *key, unsigned bit) {
    int dup = 0, n = member(nodes, object, key, &dup);
    if (n < 0) return 0;
    const Node *x = &nodes[n];
    return ((x->type == T_ARRAY || x->type == T_OBJECT) ? x->count : x->type != T_NULL) ? bit : 0;
}

SkyrimRaceMenuPresetStatus skyrim_racemenu_preset_parse(const char *json, size_t length, SkyrimRaceMenuPreset *out) {
    if (!json || !out) return SKYRIM_RACEMENU_PRESET_MALFORMED;
    if (length > SKYRIM_RACEMENU_PRESET_LIMIT) return SKYRIM_RACEMENU_PRESET_TOO_LARGE;
    if (length >= 3 && !memcmp(json, "\xef\xbb\xbf", 3)) { json += 3; length -= 3; }
    Parser s = {.p = json, .end = json + length};
    s.nodes = malloc(sizeof(Node) * MAX_NODES);
    if (!s.nodes) return SKYRIM_RACEMENU_PRESET_NO_MEMORY;
    SkyrimRaceMenuPresetStatus status = SKYRIM_RACEMENU_PRESET_MALFORMED;
    int root = parse_value(&s, 0);
    skip(&s);
    if (root < 0 || s.p != s.end || s.nodes[root].type != T_OBJECT) {
        status = s.failed ? SKYRIM_RACEMENU_PRESET_TOO_LARGE : SKYRIM_RACEMENU_PRESET_MALFORMED;
        goto done;
    }
    int dup = 0;
    int morphs = member(s.nodes, root, "morphs", &dup);
    int def = morphs >= 0 ? member(s.nodes, morphs, "default", &dup) : -1;
    int values = def >= 0 ? member(s.nodes, def, "morphs", &dup) : -1;
    if (dup) goto done;
    if (values < 0) { status = SKYRIM_RACEMENU_PRESET_NO_FACE_MORPHS; goto done; }
    status = SKYRIM_RACEMENU_PRESET_BAD_MORPHS;
    if (s.nodes[values].type != T_ARRAY || s.nodes[values].count != SKYRIM_RACEMENU_MORPHS) goto done;
    SkyrimRaceMenuPreset result = {0};
    int i = 0;
    for (int c = s.nodes[values].first; c >= 0; c = s.nodes[c].next, ++i) {
        double v = s.nodes[c].number;
        if (s.nodes[c].type != T_NUMBER || !isfinite(v) || fabs(v) > 1e6) goto done;
        result.morphs[i] = (float)v;
    }
    int presets = member(s.nodes, def, "presets", &dup);
    if (dup) goto done;
    if (presets >= 0) {
        status = SKYRIM_RACEMENU_PRESET_BAD_PRESETS;
        if (s.nodes[presets].type != T_ARRAY || s.nodes[presets].count != SKYRIM_RACEMENU_PRESETS) goto done;
        i = 0;
        for (int c = s.nodes[presets].first; c >= 0; c = s.nodes[c].next, ++i) {
            double v = s.nodes[c].number;
            if (s.nodes[c].type != T_NUMBER || v < 0 || v > 4294967295.0 || v != floor(v)) goto done;
            result.presets[i] = (unsigned)v;
        }
    }
    result.unapplied |= non_empty(s.nodes, morphs, "custom", SKYRIM_RACEMENU_UNAPPLIED_CUSTOM_MORPHS);
    result.unapplied |= non_empty(s.nodes, morphs, "sculpt", SKYRIM_RACEMENU_UNAPPLIED_SCULPT);
    result.unapplied |= non_empty(s.nodes, root, "overrides", SKYRIM_RACEMENU_UNAPPLIED_OVERRIDES);
    result.unapplied |= non_empty(s.nodes, root, "skinOverrides", SKYRIM_RACEMENU_UNAPPLIED_SKIN);
    result.unapplied |= non_empty(s.nodes, root, "bodyMorphs", SKYRIM_RACEMENU_UNAPPLIED_BODY_MORPHS);
    result.unapplied |= non_empty(s.nodes, root, "tintInfo", SKYRIM_RACEMENU_UNAPPLIED_TINTS);
    result.unapplied |= non_empty(s.nodes, root, "headParts", SKYRIM_RACEMENU_UNAPPLIED_HEAD_PARTS);
    *out = result;
    status = SKYRIM_RACEMENU_PRESET_OK;
done:
    free(s.nodes);
    return status;
}
