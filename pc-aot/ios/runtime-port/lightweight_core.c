/* SPDX-License-Identifier: GPL-3.0-only */
#include "lightweight_core.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NA NAN

/* dir -1: the value is capped at the mode's limit (lighter = smaller).
 * dir +1: the value is raised to the mode's floor (lighter = larger).
 * A NaN limit means "leave this key alone in this mode". exe_default is the
 * value the game registers in TESV for the key (read from the executable),
 * used only when neither the ini nor the caller supplies one. */
typedef struct {
    const char *section, *key;
    int dir;
    double light, lighter, exe_default;
    int exempt;                    /* startup only */
    atomic_int logged_read, changed, logged_write, last_valid, unreadable;
    atomic_uint_least64_t last_bits;
} LightKey;

#define K(s, k, d, a, b, e) { s, k, d, a, b, e, 0, 0, 0, 0, 0, 0, 0 }
static LightKey table[] = {
    K("Display", "bShadowsOnGrass", -1, 0, 0, 1),
    K("Display", "bTreesReceiveShadows", -1, 0, 0, 0),
    K("Display", "bDrawLandShadows", -1, NA, 0, 0),
    K("Display", "fShadowDistance", -1, 3000, 1500, 2500),
    K("Display", "iShadowMapResolution", -1, 2048, 1024, 1024),
    K("Display", "iShadowFilter", -1, 2, 1, 3),
    K("Display", "iMultiSample", -1, 2, 0, 0),
    K("Display", "iMaxAnisotropy", -1, 4, 2, 8),
    K("Display", "fLightLODStartFade", -1, 1500, 1000, 0),
    K("Display", "iMaxDecalsPerFrame", -1, 10, 5, 10),
    K("Display", "iMaxSkinDecalsPerFrame", -1, 3, 2, 3),
    K("Display", "iTexMipMapSkip", +1, NA, 1, 0),
    K("LOD", "fLODFadeOutMultObjects", -1, 6, 4, 5),
    K("LOD", "fLODFadeOutMultActors", -1, 8, 5, 6),
    K("LOD", "fLODFadeOutMultItems", -1, 4, 3, 3),
    K("Grass", "fGrassStartFadeDistance", -1, 3000, 1500, 0),
    K("Grass", "iMinGrassSize", +1, 40, 60, 20),
    K("Trees", "uiMaxSkinnedTreesToRender", -1, 20, 10, 40),
    K("Water", "iWaterReflectWidth", -1, 512, 256, 512),
    K("Water", "iWaterReflectHeight", -1, 512, 256, 512),
    K("Water", "bReflectLODObjects", -1, 0, 0, 0),
    K("Water", "bReflectLODTrees", -1, 0, 0, 0),
    K("Particles", "iMaxDesired", -1, 500, 250, 750),
    K("Decals", "uMaxDecals", -1, 50, 20, 100),
    K("Decals", "uMaxSkinDecals", -1, 20, 10, 35),
    K("Decals", "uMaxSkinDecalPerActor", -1, 10, 5, 20),
    K("Imagespace", "bDoDepthOfField", -1, NA, 0, 1),
};
#undef K
#define KEYS (sizeof table / sizeof table[0])

static atomic_int mode_value;

void skyrim_light_set_mode(int mode) {
    atomic_store_explicit(&mode_value, mode == 1 || mode == 2 ? mode : 0, memory_order_relaxed);
}
int skyrim_light_mode(void) { return atomic_load_explicit(&mode_value, memory_order_relaxed); }

static LightKey *find(const char *section, const char *key) {
    if (!section || !key) return NULL;
    for (size_t i = 0; i < KEYS; ++i)
        if (!strcasecmp(table[i].section, section) && !strcasecmp(table[i].key, key)) return &table[i];
    return NULL;
}

void skyrim_light_exempt(const char *section, const char *key) {
    LightKey *k = find(section, key);
    if (k) k->exempt = 1;
}

static int target_file(const char *file) {
    if (!file) return 0;
    const char *base = file, *p;
    for (p = file; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    return !strcasecmp(base, "SkyrimPrefs.ini") || !strcasecmp(base, "Skyrim.ini");
}

/* The whole text must be one finite number of sane size (optional blanks around). */
static int parse_number(const char *text, double *out) {
    if (!text) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    if (!*text) return 0;
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end || !isfinite(v) || fabs(v) > 1e9) return 0;
    *out = v;
    return 1;
}

static const char *base_name(const char *file) {
    const char *base = file, *p;
    for (p = file; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    return !strcasecmp(base, "Skyrim.ini") ? "ini" : "prefs";
}

static void format_value(const LightKey *k, double v, char *buf, size_t size) {
    if (k->key[0] == 'f') snprintf(buf, size, "%.4f", v);
    else snprintf(buf, size, "%d", (int)lround(v));
}

const char *skyrim_light_read(const char *file, const char *section, const char *key,
                              const char *current, const char *fallback, char *buf, size_t size) {
    const int mode = skyrim_light_mode();
    if (!mode) return NULL;
    if (!buf || size < 16 || !target_file(file)) return NULL;
    LightKey *k = find(section, key);
    if (!k || k->exempt) return NULL;
    const double limit = mode == 1 ? k->light : k->lighter;
    if (isnan(limit)) return NULL;

    double original;
    int from_default = 0;
    if (current) {
        if (!parse_number(current, &original)) { atomic_store_explicit(&k->unreadable, 1, memory_order_relaxed); return NULL; }
    } else if (fallback && *fallback) {
        if (!parse_number(fallback, &original)) { atomic_store_explicit(&k->unreadable, 1, memory_order_relaxed); return NULL; }
        from_default = 1;
    } else {
        original = k->exe_default;
        from_default = 1;
    }
    const double lighter = k->dir < 0 ? fmin(original, limit) : fmax(original, limit);
    const int same = fabs(lighter - original) < 1e-9;
    if (!atomic_exchange_explicit(&k->logged_read, 1, memory_order_relaxed)) {
        char to[48];
        if (same) snprintf(to, sizeof to, "same");
        else format_value(k, lighter, to, sizeof to);
        fprintf(stderr, "IOS_LIGHT read file=%s section=%s key=%s from=%.4f src=%s to=%s\n", base_name(file), k->section,
                k->key, original, from_default ? "default" : "ini", to);
    }
    if (same) return NULL;
    format_value(k, lighter, buf, size);
    double stored;
    parse_number(buf, &stored);
    uint64_t bits;
    memcpy(&bits, &stored, sizeof bits);
    atomic_store_explicit(&k->last_bits, bits, memory_order_relaxed);
    atomic_store_explicit(&k->last_valid, 1, memory_order_release);
    atomic_store_explicit(&k->changed, 1, memory_order_relaxed);
    return buf;
}

int skyrim_light_write_is_echo(const char *file, const char *section, const char *key, const char *value) {
    if (!skyrim_light_mode() || !target_file(file)) return 0;
    LightKey *k = find(section, key);
    if (!k || !atomic_load_explicit(&k->last_valid, memory_order_acquire)) return 0;
    double written;
    if (!parse_number(value, &written)) return 0;
    uint64_t bits = atomic_load_explicit(&k->last_bits, memory_order_relaxed);
    double handed_out;
    memcpy(&handed_out, &bits, sizeof handed_out);
    if (fabs(written - handed_out) >= 1e-3) return 0;
    if (!atomic_exchange_explicit(&k->logged_write, 1, memory_order_relaxed))
        fprintf(stderr, "IOS_LIGHT write_kept section=%s key=%s\n", k->section, k->key);
    return 1;
}

void skyrim_light_log_summary(void) {
    const int mode = skyrim_light_mode();
    unsigned keys = 0, read = 0, changed = 0, exempt = 0;
    char unread[1200];
    size_t used = 0;
    unread[0] = 0;
    for (size_t i = 0; i < KEYS; ++i) {
        const LightKey *k = &table[i];
        if (k->exempt) { ++exempt; continue; }
        if (isnan(mode == 1 ? k->light : k->lighter)) continue;
        ++keys;
        if (atomic_load_explicit(&k->logged_read, memory_order_relaxed)) ++read;
        else if (used + strlen(k->key) + 2 < sizeof unread)
            used += (size_t)snprintf(unread + used, sizeof unread - used, "%s%s", used ? "," : "", k->key);
        if (atomic_load_explicit(&k->changed, memory_order_relaxed)) ++changed;
    }
    if (!mode) { keys = read = changed = 0; unread[0] = 0; }
    fprintf(stderr, "IOS_LIGHT summary mode=%d keys=%u read=%u changed=%u exempt=%u unread=%s\n",
            mode, keys, read, changed, exempt, unread);
}
