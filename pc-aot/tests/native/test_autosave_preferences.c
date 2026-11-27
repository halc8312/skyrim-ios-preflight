/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "autosave_preferences.h"

static double startup_mode = -1;
static int trial_enabled;
static const char *g_game_dir = "source";
static int rt_host_path(const char *win, char *host, size_t cap) {
    assert(!strcmp(win, "C:\\Users\\Player\\Documents\\My Games\\Skyrim\\SkyrimPrefs.ini"));
    snprintf(host, cap, "profile/SkyrimPrefs.ini");
    struct stat st;
    return !stat(host, &st);
}
static int skyrim_mobile_graphics_changed(void) { return startup_mode == 0 || startup_mode == 1; }
static int skyrim_mobile_render_override(int *width, int *height) {
    if (!trial_enabled) return 0;
    *width = 1564;
    *height = 720;
    return 1;
}
static int skyrim_mobile_ini(unsigned i, const char **section, const char **key, const char **value) {
    return skyrim_autosave_ini(startup_mode, i, section, key, value);
}
#define LOG(...) ((void)0)
/* ACTUAL_INI_CODE */

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(!fclose(f));
}
static void equals_file(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb"); assert(f); char buf[2048];
    size_t n = fread(buf, 1, sizeof buf, f); assert(!ferror(f)); fclose(f);
    assert(n == strlen(expected) && !memcmp(buf, expected, n));
}
static void equals_value(IniFile *f, const char *s, const char *k, const char *v) {
    const char *found = ini_find(f,s,k); assert(found && !strcmp(found,v));
}
static void forget(void) {
    render_prefs = NULL;
    while (g_inis) { IniFile *next=g_inis->next; ini_free(g_inis); free(g_inis); g_inis=next; }
}
static void verify_profile(const char *autosave) {
    IniFile f = {0}; snprintf(f.path,sizeof f.path,"profile/SkyrimPrefs.ini"); ini_parse(&f);
    const char *keys[] = {"bSaveOnPause","bSaveOnTravel","bSaveOnWait","bSaveOnRest"};
    for (unsigned i=0;i<4;i++) equals_value(&f,"MAIN",keys[i],autosave);
    equals_value(&f,"SaveGame","fAutosaveEveryXMins","17.5");
    equals_value(&f,"SaveGame","iAutoSaveCount","3");
    equals_value(&f,"Display","iPresentInterval","0");
    equals_value(&f,"Display","iSize W","1600");
    equals_value(&f,"Display","iSize H","900");
    equals_value(&f,"Display","iMultiSample","4");
    equals_value(&f,"Grass","fGrassStartFadeDistance","4321");
    equals_value(&f,"Archive","sResourceArchiveList","Authored.bsa");
    equals_value(&f,"General","sLanguage","ENGLISH");
    equals_value(&f,"Unrelated","bSaveOnWait","7");
    unsigned count=0; for(int i=0;i<f.n;i++) if(f.e[i].key && !strcasecmp(f.e[i].sec,"MAIN")) count++;
    assert(count==5); /* Four options plus the pre-existing gamepad setting. */
    ini_free(&f);
}
int main(void) {
    assert(!mkdir("source",0700)); assert(!mkdir("profile",0700));
    const char *preset="[MAIN]\nbSaveOnPause=1\nbSaveOnTravel=0\n[Display]\niMultiSample=0\n";
    write_text("source/low.ini",preset);
    const char *original="; authored profile\r\n[Main]\r\nbSaveOnPause=1\r\nbSaveOnTravel=0\r\nbsaveonwait=1\r\nbGamepadEnable=1\r\n"
        "[SaveGame]\nfAutosaveEveryXMins=17.5\niAutoSaveCount=3\n[Display]\niPresentInterval=0\niSize W=1600\niSize H=900\niMultiSample=4\n"
        "iWaterMultisamples=4\n[Imagespace]\nbDoDepthOfField=1\n"
        "[Grass]\nfGrassStartFadeDistance=4321\n[Archive]\nsResourceArchiveList=Authored.bsa\n[General]\nsLanguage=ENGLISH\n[Unrelated]\nbSaveOnWait=7\n";
    write_text("profile/SkyrimPrefs.ini",original);
    trial_enabled = 1;
    skyrim_seed_prefs(360,200);
    IniFile *trial = ini_get("C:\\Users\\Player\\Documents\\My Games\\Skyrim\\SkyrimPrefs.ini");
    equals_value(trial,"display","isize w","1564");
    equals_value(trial,"Display","iSize H","720");
    equals_value(trial,"Display","iMultiSample","4");
    equals_file("profile/SkyrimPrefs.ini",original);
    startup_mode=0; skyrim_seed_prefs(360,200);
    verify_profile("0"); // The parsed file on disk retains 1600x900.
    forget();
    trial_enabled=0; startup_mode=-1;
    skyrim_seed_prefs(360,200); verify_profile("0");
    forget();
    write_text("profile/SkyrimPrefs.ini",original);
    const double preserve[]={-1,-2,2,0.5,NAN,INFINITY,-INFINITY};
    for(unsigned n=0;n<sizeof preserve/sizeof preserve[0];n++) {
        startup_mode=preserve[n]; skyrim_seed_prefs(360,200);
        equals_file("profile/SkyrimPrefs.ini",original);
        const char *s="unchanged",*k=s,*v=s;
        assert(!skyrim_autosave_ini(startup_mode,0,&s,&k,&v));
        assert(!strcmp(s,"unchanged") && s==k && k==v);
    }
    for(unsigned n=0;n<4;n++) {
        startup_mode=n%2; skyrim_seed_prefs(360,200); verify_profile(n%2 ? "1":"0");
        forget(); /* Next launch must read the disk file, not a test-only cache. */
        startup_mode=-1; skyrim_seed_prefs(360,200); verify_profile(n%2 ? "1":"0");
    }
    const char *s="unchanged",*k=s,*v=s;
    assert(!skyrim_autosave_ini(0,4,&s,&k,&v)); assert(s==k && k==v && !strcmp(s,"unchanged"));
    assert(!skyrim_autosave_ini(1,~0u,&s,&k,&v));
    assert(!unlink("profile/SkyrimPrefs.ini")); startup_mode=0; skyrim_seed_prefs(800,450);
    IniFile f={0}; snprintf(f.path,sizeof f.path,"profile/SkyrimPrefs.ini"); ini_parse(&f);
    equals_value(&f,"MAIN","bSaveOnPause","0"); equals_value(&f,"MAIN","bSaveOnRest","0");
    equals_value(&f,"Display","iSize W","800"); equals_value(&f,"Display","iSize H","450");
    equals_value(&f,"Display","iMultiSample","0");
    assert(!ini_find(&f,"SaveGame","fAutosaveEveryXMins"));
    ini_free(&f); forget(); equals_file("source/low.ini",preset);
    puts("autosave_profile=PASS");
}
