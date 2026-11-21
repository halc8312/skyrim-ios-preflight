/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "mod_features.h"
#include "mod_plugins.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef SKYRIM_IOS_MOD_HOOKS
#define MOD_HOOKS_IMPLEMENTED 1
#else
#define MOD_HOOKS_IMPLEMENTED 0
#endif

typedef struct {
    const char *key;
    const char *dll_keyword; /* case-insensitive part of a DLL name in SKSE/Plugins, or NULL */
    const char *plugin;      /* exact plugin file name in Data, or NULL */
    int implemented;
} FeatureRow;

/* Names follow each author's LE package. A keyword keeps the match tolerant of
 * spacing and capitalization. None of them is a statement about a version. */
static const FeatureRow rows[SKYRIM_FEATURE_COUNT] = {
    /* MOD hook builds carry the app half (mod_fuz_host.c); the guest half is checked at launch. */
    [SKYRIM_FEATURE_FUZ] = {"fuz", "fuz", NULL, MOD_HOOKS_IMPLEMENTED},
    [SKYRIM_FEATURE_PAPYRUSUTIL] = {"papyrusutil", "storageutil", NULL, 0},
    [SKYRIM_FEATURE_PRECACHE_KILLER] = {"precache", "precache", NULL, 0},
    [SKYRIM_FEATURE_RACEMENU] = {"racemenu", "chargen", NULL, 0},
    [SKYRIM_FEATURE_NIOVERRIDE] = {"nioverride", "nioverride", NULL, 0},
    [SKYRIM_FEATURE_ECE] = {"ece", NULL, "EnhancedCharacterEdit.esp", 0},
    [SKYRIM_FEATURE_ENHANCED_CAMERA] = {"enhancedcamera", "enhancedcamera", NULL, 0},
    [SKYRIM_FEATURE_SAFETY_LOAD] = {"safetyload", "safetyload", NULL, 0},
    [SKYRIM_FEATURE_HDT_PHYSICS] = {"hdtphysics", "hdtphysics", NULL, 0},
    [SKYRIM_FEATURE_HDT_HIGHHEELS] = {"hdthighheels", "highheel", NULL, 0},
};

const char *skyrim_feature_key(unsigned feature) { return feature < SKYRIM_FEATURE_COUNT ? rows[feature].key : NULL; }

int skyrim_feature_from_key(const char *key) {
    for (int i = 0; key && i < SKYRIM_FEATURE_COUNT; ++i)
        if (!strcmp(key, rows[i].key)) return i;
    return -1;
}

int skyrim_feature_implemented(unsigned feature) { return feature < SKYRIM_FEATURE_COUNT && rows[feature].implemented; }

/* 1 found directory, 0 absent, -1 link, alias or error. */
static int child_directory(const char *parent, const char *name, char *out, size_t capacity) {
    int found = skyrim_plugin_child_path(parent, name, out, capacity);
    if (found <= 0) return found;
    struct stat st;
    if (lstat(out, &st)) return errno == ENOENT ? 0 : -1;
    return S_ISDIR(st.st_mode) ? 1 : -1;
}

static int scan_root(const char *root, unsigned *mask) {
    char skse[4096], plugins[4096];
    int e = child_directory(root, "SKSE", skse, sizeof skse);
    if (e > 0) e = child_directory(skse, "Plugins", plugins, sizeof plugins);
    if (e < 0) return -1;
    if (e > 0) {
        DIR *dir = opendir(plugins);
        if (!dir) return -1;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            size_t n = strlen(entry->d_name);
            if (n < 5 || strcasecmp(entry->d_name + n - 4, ".dll")) continue;
            char path[4096];
            struct stat st;
            if (snprintf(path, sizeof path, "%s/%s", plugins, entry->d_name) >= (int)sizeof path ||
                lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
            for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f)
                if (rows[f].dll_keyword && strcasestr(entry->d_name, rows[f].dll_keyword)) *mask |= 1u << f;
        }
        closedir(dir);
    }
    for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f)
        if (rows[f].plugin && skyrim_plugin_file_present(root, rows[f].plugin)) *mask |= 1u << f;
    return 0;
}

int skyrim_features_scan(const char *const *roots, size_t count, SkyrimFeatureScan *out) {
    if (!out || (count && !roots)) return EINVAL;
    memset(out, 0, sizeof *out);
    for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f) out->features[f].winner = -1;
    for (size_t i = 0; i < count; ++i) {
        unsigned mask = 0;
        if (scan_root(roots[i], &mask) < 0) { ++out->unreadable_roots; continue; }
        for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f)
            if (mask & (1u << f)) { ++out->features[f].providers; out->features[f].winner = (int)i; }
    }
    return 0;
}
