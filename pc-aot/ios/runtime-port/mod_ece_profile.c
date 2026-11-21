/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "mod_ece_profile.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *name;
    size_t size;
    const char *sha256;
} FileIdentity;

/* The unchanged 00 Main files from the reviewed ECE 1.59 LE release. */
static const FileIdentity female_head = {
    "FemaleHeadCharGen.tri", 803424, "39f940fcf6f2b3cb3f9ffefca09e5bc82ea676ec12251e4e61cf1abd5df09475"
};
static const FileIdentity races = {
    "CME_VanillaRaces.ini", 2832, "694fb7b161e4df9857b3139e27c6376c0444a00c3fd42f4d202a9467927c3723"
};
static const FileIdentity common[] = {
    {"HumanRaces_Slider.ini", 31405, "efedb3ae0e1bd7a591b06a0b6316b19ad58ea31029f5df4caa766c3664054ea8"},
    {"baseHeadFemale.tri", 803424, "acb351463ee77cd7a68cbe227adc651f43490cb42ef405b43e5233a7a57fe57c"},
    {"CME_HeadFemale.tri", 564171, "0491beb8586d8ac705d7ca246a4890f9f826cb45e8b0382eb7ee2a557b28c312"},
    {"ECEHeadFemale.tri", 1344402, "0ef14130814522db82ead87331ba017a59b9cd27dafcdacc04bdd06e0a421508"},
    {"RANs_HeadFemale.tri", 468274, "9282f9667ffde39aba8e758fe1dc8d9e435a6883dccba8d27b38313d8518bd85"},
    {"femalehead.tri", 323838, "35319e40e7d801ec7d644c3a30db047b93228e4b0f1366c2438e840f6f1e3e47"},
};

static DIR *entries_of(int parent) {
    /* An independent descriptor leaves the caller's directory offset alone. */
    int scan = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return NULL;
    DIR *entries = fdopendir(scan);
    if (!entries) close(scan);
    return entries;
}

static SkyrimEceProfileStatus open_child(int parent, const char *name, int flags, int *child) {
    DIR *entries = entries_of(parent);
    if (!entries) return SKYRIM_ECE_PROFILE_UNREADABLE;
    SkyrimEceProfileStatus result = SKYRIM_ECE_PROFILE_MISSING;
    char leaf[64];
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (strcasecmp(name, entry->d_name)) continue;
        size_t length = strlen(entry->d_name);
        if (result == SKYRIM_ECE_PROFILE_MATCH || length >= sizeof leaf) {
            result = SKYRIM_ECE_PROFILE_UNREADABLE;
            break;
        }
        memcpy(leaf, entry->d_name, length + 1);
        result = SKYRIM_ECE_PROFILE_MATCH;
    }
    if (errno) result = SKYRIM_ECE_PROFILE_UNREADABLE;
    closedir(entries);
    if (result != SKYRIM_ECE_PROFILE_MATCH) return result;
    *child = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | flags);
    return *child < 0 ? SKYRIM_ECE_PROFILE_UNREADABLE : SKYRIM_ECE_PROFILE_MATCH;
}

static SkyrimEceProfileStatus open_directory(int root, const char *const *path, size_t count, int *directory) {
    int current = root;
    for (size_t i = 0; i < count; ++i) {
        int next;
        SkyrimEceProfileStatus result = open_child(current, path[i], O_DIRECTORY, &next);
        if (current != root) close(current);
        if (result != SKYRIM_ECE_PROFILE_MATCH) return result;
        current = next;
    }
    *directory = current;
    return SKYRIM_ECE_PROFILE_MATCH;
}

static SkyrimEceProfileStatus check_default_ini_set(int cme) {
    DIR *entries = entries_of(cme);
    if (!entries) return SKYRIM_ECE_PROFILE_UNREADABLE;
    SkyrimEceProfileStatus result = SKYRIM_ECE_PROFILE_MATCH;
    errno = 0;
    struct dirent *entry;
    /* The release enumerates top-level *.ini files, including SliderScale.ini. */
    while ((entry = readdir(entries))) {
        const char *extension = strrchr(entry->d_name, '.');
        if (extension && !strcasecmp(extension, ".ini") && strcasecmp(entry->d_name, races.name)) {
            result = SKYRIM_ECE_PROFILE_NONDEFAULT;
            break;
        }
    }
    if (errno) result = SKYRIM_ECE_PROFILE_UNREADABLE;
    closedir(entries);
    return result;
}

static bool same_digest(const uint8_t actual[32], const char *expected) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        if (hex[actual[i] >> 4] != expected[2 * i] || hex[actual[i] & 15] != expected[2 * i + 1]) return false;
    }
    return true;
}

static SkyrimEceProfileStatus check_file(int directory, const FileIdentity *file, SkyrimEceProfileDigest digest) {
    int fd;
    SkyrimEceProfileStatus result = open_child(directory, file->name, O_NONBLOCK, &fd);
    if (result != SKYRIM_ECE_PROFILE_MATCH) return result;
    struct stat st;
    uint8_t actual[32];
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        result = SKYRIM_ECE_PROFILE_UNREADABLE;
    } else if (st.st_size != (off_t)file->size) {
        result = SKYRIM_ECE_PROFILE_NONDEFAULT;
    } else if (!digest(fd, file->size, actual)) {
        result = SKYRIM_ECE_PROFILE_UNREADABLE;
    } else if (!same_digest(actual, file->sha256)) {
        result = SKYRIM_ECE_PROFILE_NONDEFAULT;
    }
    close(fd);
    return result;
}

SkyrimEceProfileStatus skyrim_ece_profile_check_female_nord(int data, SkyrimEceProfileDigest digest) {
    static const char *const character_assets[] = {"Meshes", "actors", "character", "character assets"};
    static const char *const cme_path[] = {"Meshes", "CharacterMakingExtender"};
    if (data < 0 || !digest) return SKYRIM_ECE_PROFILE_UNREADABLE;
    int directory;
    SkyrimEceProfileStatus result = open_directory(data, character_assets, 4, &directory);
    if (result != SKYRIM_ECE_PROFILE_MATCH) return result;
    result = check_file(directory, &female_head, digest);
    close(directory);
    if (result != SKYRIM_ECE_PROFILE_MATCH) return result;

    int cme;
    result = open_directory(data, cme_path, 2, &cme);
    if (result != SKYRIM_ECE_PROFILE_MATCH) return result;
    result = check_default_ini_set(cme);
    if (result == SKYRIM_ECE_PROFILE_MATCH) result = check_file(cme, &races, digest);
    if (result == SKYRIM_ECE_PROFILE_MATCH) result = open_child(cme, "commonTriRaces", O_DIRECTORY, &directory);
    close(cme);
    if (result != SKYRIM_ECE_PROFILE_MATCH) return result;
    for (size_t i = 0; i < sizeof common / sizeof common[0] && result == SKYRIM_ECE_PROFILE_MATCH; ++i) {
        result = check_file(directory, &common[i], digest);
    }
    close(directory);
    return result;
}
