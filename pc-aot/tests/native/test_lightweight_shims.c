/* SPDX-License-Identifier: GPL-3.0-only */
/* The real ini read and write functions from the staged runtime (patched by
 * prepare_runtime.patch_preferences), run against a real ini file with the real lightweight_core.c. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "lightweight_core.h"

typedef struct { uint32_t r[8]; } CPU;
static uint8_t *MEM;
#define M32(a) (*(uint32_t *)(MEM + (a)))
#define ARG(n) M32(c->r[4] + 4 + 4 * (n))
#define RET(v, nargs) do { c->r[0] = (uint32_t)(v); c->r[4] += 4 + 4 * (nargs); return; } while (0)
#define SHIM(name) static void shim_##name(CPU *c)
static inline char *GSTR(uint32_t a) { return a ? (char *)(MEM + a) : NULL; }
static const char *g_game_dir = "game";
static int rt_host_path(const char *win, char *host, size_t cap) {
    const char *base = strrchr(win, '\\');
    snprintf(host, cap, "profile/%s", base ? base + 1 : win);
    struct stat st;
    return !stat(host, &st);
}
/* ACTUAL_INI_CODE */

/* Guest memory: strings are placed at fixed addresses; the argument frame sits at 0x100. */
static uint32_t next_string = 0x1000;
static uint32_t put(const char *s) {
    if (!s) return 0;
    uint32_t at = next_string;
    memcpy(MEM + at, s, strlen(s) + 1);
    next_string += (uint32_t)strlen(s) + 1;
    return at;
}
static const char *PREFS = "C:\\Users\\Player\\Documents\\My Games\\Skyrim\\SkyrimPrefs.ini";
static const char *OTHER = "C:\\Users\\Player\\Documents\\My Games\\Skyrim\\Other.ini";
static const uint32_t OUT = 0x8000;

static void frame(CPU *c, int n, const uint32_t *args) {
    c->r[4] = 0x100;
    M32(0x100) = 0;
    for (int i = 0; i < n; ++i) M32(0x104 + 4 * i) = args[i];
}
static char *read_string(const char *file, const char *sec, const char *key, const char *def) {
    CPU c = {{0}};
    uint32_t a[6] = {put(sec), put(key), put(def), OUT, 64, put(file)};
    frame(&c, 6, a);
    shim_GetPrivateProfileStringA(&c);
    return (char *)(MEM + OUT);
}
static uint32_t read_int(const char *file, const char *sec, const char *key, uint32_t def) {
    CPU c = {{0}};
    uint32_t a[4] = {put(sec), put(key), def, put(file)};
    frame(&c, 4, a);
    shim_GetPrivateProfileIntA(&c);
    return c.r[0];
}
static void write_string(const char *file, const char *sec, const char *key, const char *val) {
    CPU c = {{0}};
    uint32_t a[4] = {put(sec), put(key), put(val), put(file)};
    frame(&c, 4, a);
    shim_WritePrivateProfileStringA(&c);
}
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(!fclose(f));
}
static char *slurp(const char *path) {
    static char buf[8192];
    FILE *f = fopen(path, "rb"); assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0; return buf;
}
#define STR(x, want) do { const char *got_ = (x); if (strcmp(got_, (want))) { fprintf(stderr, "FAIL %d: '%s' != '%s'\n", __LINE__, got_, (want)); exit(1); } } while (0)

int main(void) {
    assert(!mkdir("game", 0700)); assert(!mkdir("profile", 0700));
    MEM = calloc(1, 1 << 16);
    const char *original = "[Display]\r\nfShadowDistance=8000.0000\r\niMaxAnisotropy=16\r\niShadowMapResolution=512\r\niSize W=1170\r\n"
        "[Grass]\r\nfGrassStartFadeDistance=9000.0000\r\niMinGrassSize=20\r\n[Water]\r\nbReflectLODObjects=1\r\n";
    write_text("profile/SkyrimPrefs.ini", original);
    write_text("profile/Other.ini", original);
    write_text("profile/Skyrim.ini", "[Grass]\r\niMinGrassSize=20\r\n");

    /* Mode off (the default): every read and write is exactly the file's. */
    STR(read_string(PREFS, "Display", "fShadowDistance", "1"), "8000.0000");
    assert(read_int(PREFS, "Display", "iMaxAnisotropy", 5) == 16);
    STR(read_string(PREFS, "Display", "bShadowsOnGrass", "7"), "7");
    write_string(PREFS, "Display", "fShadowDistance", "5000.0000");
    STR(read_string(PREFS, "Display", "fShadowDistance", "1"), "5000.0000");
    write_string(PREFS, "Display", "fShadowDistance", "8000.0000");
    assert(!strcmp(slurp("profile/SkyrimPrefs.ini"), "[Display]\r\nfShadowDistance=8000.0000\r\niMaxAnisotropy=16\r\niShadowMapResolution=512\r\niSize W=1170\r\n"
        "\r\n[Grass]\r\nfGrassStartFadeDistance=9000.0000\r\niMinGrassSize=20\r\n\r\n[Water]\r\nbReflectLODObjects=1\r\n"));
    const char *baseline = slurp("profile/SkyrimPrefs.ini");
    char kept[2048]; snprintf(kept, sizeof kept, "%s", baseline);

    /* Mode 1: the game sees lowered values; the file never changes. */
    skyrim_light_set_mode(1);
    STR(read_string(PREFS, "Display", "fShadowDistance", "1"), "3000.0000");
    assert(read_int(PREFS, "Display", "iMaxAnisotropy", 5) == 4);
    assert(read_int(PREFS, "Display", "iShadowMapResolution", 1024) == 512);     /* already lighter: untouched */
    STR(read_string(PREFS, "Grass", "iMinGrassSize", "5"), "40");               /* raised: fewer, larger grass */
    STR(read_string(PREFS, "Grass", "fGrassStartFadeDistance", "0"), "3000.0000");
    STR(read_string(PREFS, "Display", "bShadowsOnGrass", "1"), "0");            /* absent in the file: uses the game's default */
    assert(read_int(PREFS, "Display", "bShadowsOnGrass", 1) == 0);
    assert(read_int(PREFS, "Water", "bReflectLODObjects", 1) == 0);
    assert(read_int(PREFS, "Display", "iSize W", 0) == 1170);                    /* not a lightweight key */
    STR(read_string(OTHER, "Display", "fShadowDistance", "1"), "8000.0000");     /* other ini files are not touched */
    STR(read_string("C:\\Users\\Player\\Documents\\My Games\\Skyrim\\Skyrim.ini", "Grass", "iMinGrassSize", "5"), "40");
    /* The game saves its prefs with the values it saw: skipped, so the file keeps the player's own values. */
    write_string(PREFS, "Display", "fShadowDistance", "3000.0000");
    write_string(PREFS, "Display", "iMaxAnisotropy", "4");
    write_string(PREFS, "Grass", "iMinGrassSize", "40");
    write_string(PREFS, "Display", "bShadowsOnGrass", "0");
    assert(!strcmp(slurp("profile/SkyrimPrefs.ini"), kept));
    assert(!strstr(slurp("profile/SkyrimPrefs.ini"), "bShadowsOnGrass"));
    /* A value the player really changed is written. */
    write_string(PREFS, "Display", "fShadowDistance", "6000.0000");
    assert(strstr(slurp("profile/SkyrimPrefs.ini"), "fShadowDistance=6000.0000"));
    write_string(PREFS, "Display", "fShadowDistance", "8000.0000");

    /* Mode 2 lowers further; off again returns every value to the player's own. */
    skyrim_light_set_mode(2);
    STR(read_string(PREFS, "Display", "fShadowDistance", "1"), "1500.0000");
    assert(read_int(PREFS, "Display", "iMaxAnisotropy", 5) == 2);
    skyrim_light_set_mode(0);
    STR(read_string(PREFS, "Display", "fShadowDistance", "1"), "8000.0000");
    assert(read_int(PREFS, "Display", "iMaxAnisotropy", 5) == 16);
    STR(read_string(PREFS, "Grass", "iMinGrassSize", "5"), "20");
    assert(!strcmp(slurp("profile/SkyrimPrefs.ini"), kept));

    /* A key the player chose explicitly is exempt. */
    skyrim_light_exempt("Display", "iMaxAnisotropy");
    skyrim_light_set_mode(1);
    assert(read_int(PREFS, "Display", "iMaxAnisotropy", 5) == 16);
    STR(read_string(PREFS, "Display", "fShadowDistance", "1"), "3000.0000");
    puts("lightweight_shims=PASS");
    return 0;
}
