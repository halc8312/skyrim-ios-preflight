/* SPDX-License-Identifier: GPL-3.0-only */
#include "lightweight_core.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

/* Independent copy of the expected table (not shared with the implementation):
 * section, key, dir, light, lighter, exe default. NAN = untouched in that mode. */
typedef struct { const char *section, *key; int dir; double light, lighter, def; } Expect;
static const Expect expect[] = {
    {"Display", "bShadowsOnGrass", -1, 0, 0, 1},
    {"Display", "bTreesReceiveShadows", -1, 0, 0, 0},
    {"Display", "bDrawLandShadows", -1, NAN, 0, 0},
    {"Display", "fShadowDistance", -1, 3000, 1500, 2500},
    {"Display", "iShadowMapResolution", -1, 2048, 1024, 1024},
    {"Display", "iShadowFilter", -1, 2, 1, 3},
    {"Display", "iMultiSample", -1, 2, 0, 0},
    {"Display", "iMaxAnisotropy", -1, 4, 2, 8},
    {"Display", "fLightLODStartFade", -1, 1500, 1000, 0},
    {"Display", "iMaxDecalsPerFrame", -1, 10, 5, 10},
    {"Display", "iMaxSkinDecalsPerFrame", -1, 3, 2, 3},
    {"Display", "iTexMipMapSkip", +1, NAN, 1, 0},
    {"LOD", "fLODFadeOutMultObjects", -1, 6, 4, 5},
    {"LOD", "fLODFadeOutMultActors", -1, 8, 5, 6},
    {"LOD", "fLODFadeOutMultItems", -1, 4, 3, 3},
    {"Grass", "fGrassStartFadeDistance", -1, 3000, 1500, 0},
    {"Grass", "iMinGrassSize", +1, 40, 60, 20},
    {"Trees", "uiMaxSkinnedTreesToRender", -1, 20, 10, 40},
    {"Water", "iWaterReflectWidth", -1, 512, 256, 512},
    {"Water", "iWaterReflectHeight", -1, 512, 256, 512},
    {"Water", "bReflectLODObjects", -1, 0, 0, 0},
    {"Water", "bReflectLODTrees", -1, 0, 0, 0},
    {"Particles", "iMaxDesired", -1, 500, 250, 750},
    {"Decals", "uMaxDecals", -1, 50, 20, 100},
    {"Decals", "uMaxSkinDecals", -1, 20, 10, 35},
    {"Decals", "uMaxSkinDecalPerActor", -1, 10, 5, 20},
    {"Imagespace", "bDoDepthOfField", -1, NAN, 0, 1},
};
#define N (sizeof expect / sizeof expect[0])

static const char *const PREFS = "C:\\Users\\Player\\Documents\\My Games\\Skyrim\\SkyrimPrefs.ini";
static const char *const INI = "C:\\Users\\Player\\Documents\\My Games\\Skyrim\\Skyrim.ini";

static void text(const Expect *e, double v, char *out) {
    if (e->key[0] == 'f') snprintf(out, 48, "%.4f", v);
    else snprintf(out, 48, "%d", (int)lround(v));
}

static void test_off_changes_nothing(void) {
    CHECK(skyrim_light_mode() == 0);
    char buf[48];
    for (size_t i = 0; i < N; ++i)
        for (int file = 0; file < 2; ++file)
            for (int c = 0; c < 4; ++c) {
                const char *cur = c == 0 ? NULL : c == 1 ? "99999" : c == 2 ? "0" : "garbage";
                CHECK(skyrim_light_read(file ? INI : PREFS, expect[i].section, expect[i].key, cur, "7", buf, sizeof buf) == NULL);
                CHECK(skyrim_light_write_is_echo(file ? INI : PREFS, expect[i].section, expect[i].key, "0") == 0);
            }
    skyrim_light_set_mode(7);          /* anything but 1 or 2 is off */
    CHECK(skyrim_light_mode() == 0);
    skyrim_light_set_mode(-1);
    CHECK(skyrim_light_mode() == 0);
    CHECK(skyrim_light_read(PREFS, "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
}

static void test_mode(int mode) {
    skyrim_light_set_mode(mode);
    CHECK(skyrim_light_mode() == mode);
    char buf[48], want[48];
    for (size_t i = 0; i < N; ++i) {
        const Expect *e = &expect[i];
        const double limit = mode == 1 ? e->light : e->lighter;
        for (int file = 0; file < 2; ++file) {
            const char *path = file ? INI : PREFS;
            if (isnan(limit)) {
                CHECK(skyrim_light_read(path, e->section, e->key, "123456", NULL, buf, sizeof buf) == NULL);
                CHECK(skyrim_light_read(path, e->section, e->key, NULL, NULL, buf, sizeof buf) == NULL);
                continue;
            }
            const double heavier = e->dir < 0 ? limit + 1000 : limit - 1000;
            const double lighter_value = e->dir < 0 ? limit - 1 : limit + 1;
            char current[48];
            /* heavier than the limit: replaced by the limit */
            text(e, heavier, current);
            const char *got = skyrim_light_read(path, e->section, e->key, current, "", buf, sizeof buf);
            CHECK(got == buf);
            text(e, limit, want);
            CHECK(strcmp(got, want) == 0);
            /* the game writes the same value back: skip; another value: write */
            CHECK(skyrim_light_write_is_echo(path, e->section, e->key, want) == 1);
            if (e->key[0] == 'f') { char again[48]; snprintf(again, sizeof again, "%.6f", limit); CHECK(skyrim_light_write_is_echo(path, e->section, e->key, again) == 1); }
            text(e, heavier, current);
            CHECK(skyrim_light_write_is_echo(path, e->section, e->key, current) == 0);
            /* equal to the limit, or already lighter: left alone */
            text(e, limit, current);
            CHECK(skyrim_light_read(path, e->section, e->key, current, NULL, buf, sizeof buf) == NULL);
            text(e, lighter_value, current);
            CHECK(skyrim_light_read(path, e->section, e->key, current, NULL, buf, sizeof buf) == NULL);
            /* absent: the game's own default, else the executable's registered default */
            text(e, heavier, current);
            got = skyrim_light_read(path, e->section, e->key, NULL, current, buf, sizeof buf);
            CHECK(got == buf && strcmp(got, want) == 0);
            const int exe_is_heavier = e->dir < 0 ? e->def > limit : e->def < limit;
            got = skyrim_light_read(path, e->section, e->key, NULL, NULL, buf, sizeof buf);
            if (exe_is_heavier) CHECK(got == buf && strcmp(got, want) == 0);
            else CHECK(got == NULL);
            got = skyrim_light_read(path, e->section, e->key, NULL, "", buf, sizeof buf);
            if (exe_is_heavier) CHECK(got == buf); else CHECK(got == NULL);
            /* not a number: left alone, never guessed */
            CHECK(skyrim_light_read(path, e->section, e->key, "abc", NULL, buf, sizeof buf) == NULL);
            CHECK(skyrim_light_read(path, e->section, e->key, "12abc", NULL, buf, sizeof buf) == NULL);
            CHECK(skyrim_light_read(path, e->section, e->key, "", NULL, buf, sizeof buf) == NULL);
            CHECK(skyrim_light_read(path, e->section, e->key, "nan", NULL, buf, sizeof buf) == NULL);
            CHECK(skyrim_light_read(path, e->section, e->key, "1e30", NULL, buf, sizeof buf) == NULL);
        }
    }
}

static void test_selection_rules(void) {
    skyrim_light_set_mode(1);
    char buf[48];
    /* spot values from the plan */
    CHECK(strcmp(skyrim_light_read(PREFS, "Display", "fShadowDistance", "8000.0000", NULL, buf, sizeof buf), "3000.0000") == 0);
    CHECK(skyrim_light_read(PREFS, "Display", "fShadowDistance", "2000", NULL, buf, sizeof buf) == NULL);
    CHECK(strcmp(skyrim_light_read(PREFS, "Grass", "iMinGrassSize", "20", NULL, buf, sizeof buf), "40") == 0);
    CHECK(skyrim_light_read(PREFS, "Grass", "iMinGrassSize", "50", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, "Display", "iTexMipMapSkip", "0", NULL, buf, sizeof buf) == NULL);   /* not in mode 1 */
    CHECK(strcmp(skyrim_light_read(PREFS, "Display", "iMaxAnisotropy", " 16 ", NULL, buf, sizeof buf), "4") == 0);
    CHECK(skyrim_light_read(PREFS, "Display", "iMaxAnisotropy", "2.9", NULL, buf, sizeof buf) == NULL);
    skyrim_light_set_mode(2);
    CHECK(strcmp(skyrim_light_read(PREFS, "Display", "iTexMipMapSkip", "0", NULL, buf, sizeof buf), "1") == 0);
    CHECK(skyrim_light_read(PREFS, "Display", "iTexMipMapSkip", "2", NULL, buf, sizeof buf) == NULL);
    CHECK(strcmp(skyrim_light_read(PREFS, "Grass", "iMinGrassSize", "50", NULL, buf, sizeof buf), "60") == 0);
    CHECK(strcmp(skyrim_light_read(PREFS, "Imagespace", "bDoDepthOfField", "1", NULL, buf, sizeof buf), "0") == 0);
    /* names compare without case, in either file */
    CHECK(strcmp(skyrim_light_read(INI, "display", "FSHADOWDISTANCE", "8000", NULL, buf, sizeof buf), "1500.0000") == 0);
    CHECK(strcmp(skyrim_light_read("skyrimprefs.INI", "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf), "1500.0000") == 0);
    CHECK(strcmp(skyrim_light_read("/var/mobile/Skyrim/SkyrimPrefs.ini", "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf), "1500.0000") == 0);
    /* other files and other keys are never touched */
    CHECK(skyrim_light_read("C:\\Game\\Data\\Other.ini", "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read("C:\\Game\\SKSE\\Plugins\\SkyrimPrefs.ini.bak", "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read("C:\\Game\\Data\\SkyrimPrefs.ini\\x.ini", "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, "Display", "iSize W", "5000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, "Audio", "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(NULL, "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, NULL, "fShadowDistance", "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, "Display", NULL, "8000", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, "Display", "fShadowDistance", "8000", NULL, NULL, 0) == NULL);
    CHECK(skyrim_light_read(PREFS, "Display", "fShadowDistance", "8000", NULL, buf, 4) == NULL);   /* too small to hold it */
    /* an echo needs a value that was handed out for that very key */
    CHECK(skyrim_light_write_is_echo("C:\\Game\\Data\\Other.ini", "Display", "fShadowDistance", "1500") == 0);
    CHECK(skyrim_light_write_is_echo(PREFS, "Display", "iSize W", "1500") == 0);
    CHECK(skyrim_light_write_is_echo(PREFS, "Display", "fShadowDistance", "abc") == 0);
    CHECK(skyrim_light_write_is_echo(PREFS, "Display", "fShadowDistance", "1500.0000") == 1);
    CHECK(skyrim_light_write_is_echo(PREFS, "Display", "fShadowDistance", "1600") == 0);
    CHECK(skyrim_light_write_is_echo(PREFS, "Display", "fShadowDistance", NULL) == 0);
    skyrim_light_log_summary();
}

static void *hammer(void *arg) {
    (void)arg;
    char buf[48];
    for (int i = 0; i < 10000; ++i) {
        const char *got = skyrim_light_read(PREFS, "Display", "fShadowDistance", "8000", NULL, buf, sizeof buf);
        if (!got || strcmp(got, "1500.0000")) exit(2);
        if (!skyrim_light_write_is_echo(PREFS, "Display", "fShadowDistance", "1500")) exit(3);
        if (skyrim_light_read(PREFS, "Display", "fLODFadeOutMultItems", "2", NULL, buf, sizeof buf)) exit(4);
    }
    return NULL;
}

static void test_threads(void) {
    skyrim_light_set_mode(2);
    pthread_t t[8];
    for (int i = 0; i < 8; ++i) CHECK(pthread_create(&t[i], NULL, hammer, NULL) == 0);
    for (int i = 0; i < 8; ++i) CHECK(pthread_join(t[i], NULL) == 0);
}

static void test_exempt_last(void) {
    skyrim_light_set_mode(1);
    char buf[48];
    CHECK(skyrim_light_read(PREFS, "Particles", "iMaxDesired", "750", NULL, buf, sizeof buf) != NULL);
    skyrim_light_exempt("particles", "IMAXDESIRED");
    skyrim_light_exempt("Nope", "Nothing");           /* unknown keys are ignored */
    skyrim_light_exempt(NULL, NULL);
    CHECK(skyrim_light_read(PREFS, "Particles", "iMaxDesired", "750", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(INI, "Particles", "iMaxDesired", "750", NULL, buf, sizeof buf) == NULL);
    CHECK(skyrim_light_read(PREFS, "Decals", "uMaxDecals", "250", NULL, buf, sizeof buf) != NULL);   /* neighbours unaffected */
    skyrim_light_log_summary();
    skyrim_light_set_mode(0);
    skyrim_light_log_summary();
    CHECK(skyrim_light_read(PREFS, "Decals", "uMaxDecals", "250", NULL, buf, sizeof buf) == NULL);
}

int main(void) {
    test_off_changes_nothing();
    test_mode(1);
    test_mode(2);
    test_selection_rules();
    test_threads();
    test_exempt_last();
    puts("lightweight_core=PASS");
    return 0;
}
