/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "mod_features.h"
#include "mod_plugins.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

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
    int implemented;
} FeatureRow;

/* Names follow each author's LE package. A keyword keeps the match tolerant of
 * spacing and capitalization. None of them is a statement about a version. */
static const FeatureRow rows[SKYRIM_FEATURE_COUNT] = {
    /* MOD hook builds carry the app half (mod_fuz_host.c); the guest half is checked at launch. */
    [SKYRIM_FEATURE_FUZ] = {"fuz", "fuz", NULL, MOD_HOOKS_IMPLEMENTED},
    [SKYRIM_FEATURE_PAPYRUSUTIL] = {"papyrusutil", "storageutil", NULL, PAPYRUSUTIL_IMPLEMENTED},
    [SKYRIM_FEATURE_PRECACHE_KILLER] = {"precache", "precache", NULL, MOD_HOOKS_IMPLEMENTED},
    [SKYRIM_FEATURE_RACEMENU] = {"racemenu", "chargen", NULL, 0},
    [SKYRIM_FEATURE_NIOVERRIDE] = {"nioverride", NULL, NULL, 0},
    [SKYRIM_FEATURE_ECE] = {"ece", NULL, "EnhancedCharacterEdit.esp", 0},
    [SKYRIM_FEATURE_ENHANCED_CAMERA] = {"enhancedcamera", "enhancedcamera", NULL, 0},
    [SKYRIM_FEATURE_SAFETY_LOAD] = {"safetyload", "safetyload", NULL, 1},
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

static int regular_file(const char *root, const char *name, char *path, size_t capacity) {
    int found = skyrim_plugin_child_path(root, name, path, capacity);
    if (found <= 0) return found;
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) ? 1 : -1;
}

static int skse_dll_path(const char *data, const char *name, char *path, size_t capacity) {
    if (!data || !path || !capacity) return -1;
    char skse[4096], plugins[4096];
    int found = child_directory(data, "SKSE", skse, sizeof skse);
    if (found > 0) found = child_directory(skse, "Plugins", plugins, sizeof plugins);
    return found > 0 ? regular_file(plugins, name, path, capacity) : found;
}

int skyrim_precache_killer_path(const char *data, char *path, size_t capacity) {
    return skse_dll_path(data, "showRaceMenu_preCacheKiller.dll", path, capacity);
}

int skyrim_safety_load_path(const char *data, char *path, size_t capacity) {
    return skse_dll_path(data, "SafetyLoad.dll", path, capacity);
}

static uint32_t read_u16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t read_u32(const uint8_t *p) {
    return read_u16(p) | read_u16(p + 2) << 16;
}

static int read_at(int fd, uint64_t length, uint64_t offset, uint8_t *bytes, size_t count) {
    return offset <= length && count <= length - offset &&
           pread(fd, bytes, count, (off_t)offset) == (ssize_t)count;
}

static const uint8_t *span(const uint8_t *resource, size_t length, uint32_t offset, size_t count) {
    return offset <= length && count <= length - offset ? resource + offset : NULL;
}

static const uint8_t *resource_children(const uint8_t *resource, size_t length, uint32_t offset,
                                        unsigned *count) {
    const uint8_t *header = span(resource, length, offset, 16);
    if (!header) return NULL;
    *count = read_u16(header + 12) + read_u16(header + 14);
    return span(resource, length, offset + 16, (size_t)*count * 8);
}

static int fixed_version(const uint8_t *resource, size_t length, uint32_t rva,
                         uint32_t entry_offset, uint16_t version[4]) {
    static const char key[] = "VS_VERSION_INFO";
    const uint8_t *entry = span(resource, length, entry_offset, 16);
    if (!entry || read_u32(entry) < rva) return 0;
    uint32_t size = read_u32(entry + 4);
    const uint8_t *value = span(resource, length, read_u32(entry) - rva, size);
    if (!value || size < 92 || read_u16(value) < 92 || read_u16(value) > size ||
        read_u16(value + 2) != 52 || read_u16(value + 4) != 0 ||
        read_u32(value + 40) != 0xfeef04bdu) return 0;
    for (size_t i = 0; i < sizeof key; ++i)
        if (read_u16(value + 6 + i * 2) != (unsigned char)key[i]) return 0;
    uint32_t ms = read_u32(value + 48), ls = read_u32(value + 52);
    version[0] = (uint16_t)(ms >> 16);
    version[1] = (uint16_t)ms;
    version[2] = (uint16_t)(ls >> 16);
    version[3] = (uint16_t)ls;
    return 1;
}

/* PE resource directories have three levels (type, name, language). The file
 * version should be independent of language, so conflicting entries are unknown. */
static int resource_version(const uint8_t *resource, size_t length, uint32_t rva, uint16_t version[4]) {
    const uint32_t directory = 0x80000000u;
    unsigned types = 0;
    const uint8_t *type = resource_children(resource, length, 0, &types);
    if (!type) return 0;
    uint32_t name_offset = 0;
    for (unsigned t = 0; t < types; ++t, type += 8) {
        if (read_u32(type) != 16) continue; /* RT_VERSION */
        if (name_offset || !(read_u32(type + 4) & directory)) return 0;
        name_offset = read_u32(type + 4);
    }
    if (!name_offset) return 0;
    unsigned names = 0;
    const uint8_t *name = resource_children(resource, length, name_offset & ~directory, &names);
    if (!name || !names || names > 16) return 0;
    int known = 0;
    for (unsigned n = 0; n < names; ++n, name += 8) {
        uint32_t language_offset = read_u32(name + 4);
        if (!(language_offset & directory)) return 0;
        unsigned languages = 0;
        const uint8_t *language = resource_children(resource, length, language_offset & ~directory, &languages);
        if (!language || !languages || languages > 32) return 0;
        for (unsigned i = 0; i < languages; ++i, language += 8) {
            uint16_t found[4];
            uint32_t entry = read_u32(language + 4);
            if ((entry & directory) || !fixed_version(resource, length, rva, entry, found)) return 0;
            if (known && memcmp(version, found, sizeof found)) return 0;
            memcpy(version, found, sizeof found);
            known = 1;
        }
    }
    return known;
}

static int pe_file_version(int fd, uint64_t length, uint16_t version[4]) {
    uint8_t dos[64], coff[24], optional[2], directory[8], entries[4];
    if (!read_at(fd, length, 0, dos, sizeof dos) || memcmp(dos, "MZ", 2)) return 0;
    const uint64_t pe = read_u32(dos + 60);
    if (!read_at(fd, length, pe, coff, sizeof coff) || memcmp(coff, "PE\0\0", 4)) return 0;
    unsigned section_count = read_u16(coff + 6), optional_size = read_u16(coff + 20);
    const uint64_t optional_start = pe + sizeof coff;
    if (!section_count || section_count > 96 || optional_size < sizeof optional ||
        !read_at(fd, length, optional_start, optional, sizeof optional)) return 0;
    unsigned magic = read_u16(optional);
    unsigned table = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
    if (!table || optional_size < table + 24 ||
        !read_at(fd, length, optional_start + table - 4, entries, sizeof entries) ||
        read_u32(entries) < 3 ||
        !read_at(fd, length, optional_start + table + 16, directory, sizeof directory)) return 0;
    uint32_t rva = read_u32(directory), size = read_u32(directory + 4);
    if (!rva || !size || size > (1u << 20)) return 0;
    uint64_t offset = 0;
    for (unsigned i = 0; i < section_count; ++i) {
        uint8_t section[40];
        if (!read_at(fd, length, optional_start + optional_size + i * sizeof section,
                     section, sizeof section)) return 0;
        uint32_t start = read_u32(section + 12), raw_size = read_u32(section + 16);
        if (rva < start || rva - start >= raw_size || size > raw_size - (rva - start)) continue;
        if (offset) return 0; /* ambiguous RVA mapping */
        offset = (uint64_t)read_u32(section + 20) + (rva - start);
    }
    if (!offset || offset > length || size > length - offset) return 0;
    uint8_t *resource = malloc(size);
    if (!resource) return 0;
    int known = read_at(fd, length, offset, resource, size) && resource_version(resource, size, rva, version);
    free(resource);
    return known;
}

int skyrim_nioverride_file_version(const char *root, uint16_t version[4]) {
    if (!root || !version) return 0;
    char skse[4096], plugins[4096], path[4096];
    if (child_directory(root, "SKSE", skse, sizeof skse) <= 0 ||
        child_directory(skse, "Plugins", plugins, sizeof plugins) <= 0 ||
        regular_file(plugins, "nioverride.dll", path, sizeof path) <= 0) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    uint16_t found[4];
    int known = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
                pe_file_version(fd, (uint64_t)st.st_size, found);
    close(fd);
    if (known) memcpy(version, found, sizeof found);
    return known;
}

static int scan_root(const char *root, unsigned *mask) {
    char skse[4096], plugins[4096];
    int e = child_directory(root, "SKSE", skse, sizeof skse);
    if (e > 0) e = child_directory(skse, "Plugins", plugins, sizeof plugins);
    if (e < 0) return -1;
    int uncertain = 0;
    if (e > 0) {
        DIR *dir = opendir(plugins);
        if (!dir) return -1;
        unsigned ni_aliases = 0;
        int ni_regular = 0;
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            size_t n = strlen(entry->d_name);
            if (n < 5 || strcasecmp(entry->d_name + n - 4, ".dll")) continue;
            int is_nioverride = !strcasecmp(entry->d_name, "nioverride.dll");
            ni_aliases += (unsigned)is_nioverride;
            char path[4096];
            struct stat st;
            if (snprintf(path, sizeof path, "%s/%s", plugins, entry->d_name) >= (int)sizeof path ||
                lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
            if (is_nioverride) ni_regular = 1;
            for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f)
                if (rows[f].dll_keyword && strcasestr(entry->d_name, rows[f].dll_keyword)) *mask |= 1u << f;
        }
        closedir(dir);
        if (ni_aliases == 1 && ni_regular) *mask |= 1u << SKYRIM_FEATURE_NIOVERRIDE;
        else if (ni_aliases) uncertain = 1;
    }
    for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f)
        if (rows[f].plugin && skyrim_plugin_file_present(root, rows[f].plugin)) *mask |= 1u << f;
    return uncertain;
}

int skyrim_features_scan(const char *const *roots, size_t count, bool first_is_original, SkyrimFeatureScan *out) {
    if (!out || (count && !roots) || (first_is_original && !count)) return EINVAL;
    memset(out, 0, sizeof *out);
    for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f) out->features[f].winner = -1;
    for (size_t i = 0; i < count; ++i) {
        unsigned mask = 0;
        int result = scan_root(roots[i], &mask);
        if (result) ++out->unreadable_roots;
        if (result < 0) continue;
        for (int f = 0; f < SKYRIM_FEATURE_COUNT; ++f) {
            if (!(mask & (1u << f))) continue;
            SkyrimFeatureState *feature = &out->features[f];
            ++feature->providers;
            const bool kept_original = f == SKYRIM_FEATURE_NIOVERRIDE && first_is_original && feature->winner == 0;
            if (!kept_original) feature->winner = (int)i;
        }
    }
    return 0;
}
