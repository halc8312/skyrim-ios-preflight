/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "mod_features.h"
#include "mod_plugins.h"
#include "pe_image.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
#define PAPYRUSUTIL_IMPLEMENTED 1
#else
#define PAPYRUSUTIL_IMPLEMENTED 0
#endif
#ifdef SKYRIM_IOS_MOD_HOOKS
#define MOD_HOOKS_IMPLEMENTED 1
#else
#define MOD_HOOKS_IMPLEMENTED 0
#endif

typedef struct {
    const char *key;
    const char *dll_keyword; /* case-insensitive part of a DLL name in SKSE/Plugins, or NULL */
    const char *plugin;      /* exact plugin file name in Data, or NULL */
    const char *root_file;   /* a regular file at the Data root (any extension), or NULL */
    int implemented;
} FeatureRow;

/* Names follow each author's LE package. A keyword keeps the match tolerant of
 * spacing and capitalization. None of them is a statement about a version. */
static const FeatureRow rows[SKYRIM_FEATURE_COUNT] = {
    /* MOD hook builds carry the app half (mod_fuz_host.c); the guest half is checked at launch. */
    [SKYRIM_FEATURE_FUZ] = {"fuz", "fuz", NULL, NULL, MOD_HOOKS_IMPLEMENTED},
    [SKYRIM_FEATURE_PAPYRUSUTIL] = {"papyrusutil", "storageutil", NULL, NULL, PAPYRUSUTIL_IMPLEMENTED},
    [SKYRIM_FEATURE_PRECACHE_KILLER] = {"precache", "precache", NULL, NULL, 0},
    [SKYRIM_FEATURE_RACEMENU] = {"racemenu", "chargen", NULL, NULL, 0},
    [SKYRIM_FEATURE_NIOVERRIDE] = {"nioverride", "nioverride", NULL, NULL, 0},
    [SKYRIM_FEATURE_ECE] = {"ece", NULL, "EnhancedCharacterEdit.esp", NULL, 0},
    [SKYRIM_FEATURE_ENHANCED_CAMERA] = {"enhancedcamera", "enhancedcamera", NULL, NULL, 0},
    [SKYRIM_FEATURE_SAFETY_LOAD] = {"safetyload", "safetyload", NULL, NULL, 0},
    [SKYRIM_FEATURE_HDT_PHYSICS] = {"hdtphysics", "hdtphysics", NULL, NULL, 0},
    [SKYRIM_FEATURE_HDT_HIGHHEELS] = {"hdthighheels", "highheel", NULL, NULL, 0},
    /* Found now; the renderer is Codex's integration (ENB_CODEX_HANDOFF.md), so no switch yet. */
    [SKYRIM_FEATURE_ENB] = {"enb", NULL, NULL, "enbseries.ini", 0},
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
    for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f) {
        char path[4096];
        struct stat st;
        if (rows[f].root_file && skyrim_plugin_child_path(root, rows[f].root_file, path, sizeof path) == 1 &&
            !lstat(path, &st) && S_ISREG(st.st_mode)) *mask |= 1u << f;
    }
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

enum { FEATURE_DLL_MAX_BYTES = 32 * 1024 * 1024 };

int skyrim_feature_dll_version(const char *root, unsigned feature, uint16_t version[4]) {
    if (!root || !version || feature >= SKYRIM_FEATURE_COUNT || !rows[feature].dll_keyword) return EINVAL;
    memset(version, 0, 4 * sizeof version[0]);
    char skse[4096], plugins[4096], path[4096] = "";
    int e = child_directory(root, "SKSE", skse, sizeof skse);
    if (e > 0) e = child_directory(skse, "Plugins", plugins, sizeof plugins);
    if (e < 0) return EIO;
    if (!e) return ENOENT;
    DIR *dir = opendir(plugins);
    if (!dir) return EIO;
    unsigned matches = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcasecmp(entry->d_name + n - 4, ".dll") || !strcasestr(entry->d_name, rows[feature].dll_keyword))
            continue;
        char candidate[4096];
        struct stat st;
        if (snprintf(candidate, sizeof candidate, "%s/%s", plugins, entry->d_name) >= (int)sizeof candidate ||
            lstat(candidate, &st) || !S_ISREG(st.st_mode)) continue;
        if (++matches == 1) memcpy(path, candidate, sizeof path);
    }
    closedir(dir);
    if (!matches) return ENOENT;
    if (matches > 1) return EMLINK;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return EIO;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return EIO; }
    if (st.st_size > FEATURE_DLL_MAX_BYTES) { close(fd); return EFBIG; }
    uint8_t *bytes = malloc(st.st_size ? (size_t)st.st_size : 1);
    size_t have = 0;
    while (bytes && have < (size_t)st.st_size) {
        const ssize_t got = read(fd, bytes + have, (size_t)st.st_size - have);
        if (got <= 0) break;
        have += (size_t)got;
    }
    close(fd);
    SksePeVersion pe;
    const int ok = bytes && have == (size_t)st.st_size && skse_pe_version_info(bytes, have, &pe) == SKSE_PE_OK;
    free(bytes);
    if (!ok) return EIO;
    memcpy(version, pe.file, sizeof pe.file);
    return 0;
}
