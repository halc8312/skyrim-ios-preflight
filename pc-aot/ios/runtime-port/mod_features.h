/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* First-wave MOD features that a profile can opt into. Detection is by the
 * marker file names in the selected packages and never runs or modifies a
 * file. A name match says a plugin is present, not that its version is the
 * one the port was checked against. Features are off unless chosen. */
typedef enum {
    SKYRIM_FEATURE_FUZ,
    SKYRIM_FEATURE_PAPYRUSUTIL,
    SKYRIM_FEATURE_PRECACHE_KILLER,
    SKYRIM_FEATURE_RACEMENU,
    SKYRIM_FEATURE_NIOVERRIDE,
    SKYRIM_FEATURE_ECE,
    SKYRIM_FEATURE_ENHANCED_CAMERA,
    SKYRIM_FEATURE_SAFETY_LOAD,
    SKYRIM_FEATURE_HDT_PHYSICS,
    SKYRIM_FEATURE_HDT_HIGHHEELS,
    SKYRIM_FEATURE_ENB,   /* enbseries.ini in a package's Data root (docs/ENB_PLAN.md) */
    SKYRIM_FEATURE_COUNT
} SkyrimFeature;

typedef struct {
    unsigned providers; /* roots holding a marker for this feature */
    int winner;         /* index of the selected root, -1 when none */
} SkyrimFeatureState;

typedef struct {
    SkyrimFeatureState features[SKYRIM_FEATURE_COUNT];
    unsigned unreadable_roots; /* skipped: ambiguous SKSE/Plugins names or I/O errors */
} SkyrimFeatureScan;

/* roots are Data folders in overwrite order. Set first_is_original when
 * roots[0] is the original Data folder. An original NiOverride file is kept
 * ahead of any copy in a package, matching deployment's DLL protection.
 * A missing or unreadable version resource leaves its selection unchanged.
 * Links are not followed. Returns 0, or EINVAL. */
int skyrim_features_scan(const char *const *roots, size_t count, bool first_is_original, SkyrimFeatureScan *out);

/* On demand, read the selected root's NiOverride DLL fixed file version.
 * Returns 1 when available, 0 for a missing or unusable resource. */
int skyrim_nioverride_file_version(const char *root, uint16_t version[4]);

/* Locate the reviewed package's DLL under one already selected Data view.
 * Returns 1 when found, 0 when absent, -1 for ambiguous names or unsafe files. */
int skyrim_precache_killer_path(const char *data, char *path, size_t capacity);
int skyrim_safety_load_path(const char *data, char *path, size_t capacity);

/* Stable ASCII key used in saved profile state and accessibility identifiers. */
const char *skyrim_feature_key(unsigned feature);
/* Index for a key, or -1. */
int skyrim_feature_from_key(const char *key);
/* 1 when the port has an implementation behind the feature's switch. */
int skyrim_feature_implemented(unsigned feature);
#ifdef __cplusplus
}
#endif
