/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* RaceMenu LE 3.4.5 .jslot: reads only the stock face sliders
 * (morphs.default.morphs, the 19 floats of TESNPC::FaceMorphs::option[] in
 * game order) and the four face-preset words. Everything else the file can
 * hold is only reported as not applied. */
enum { SKYRIM_RACEMENU_MORPHS = 19, SKYRIM_RACEMENU_PRESETS = 4,
       SKYRIM_RACEMENU_PRESET_LIMIT = 1024 * 1024 };
/* Index in morphs[] of the first supported slider: Nose Height (FaceMorphList::kMorph_NoseDownUp). */
enum { SKYRIM_RACEMENU_NOSE_HEIGHT = 1 };

enum {
    SKYRIM_RACEMENU_UNAPPLIED_CUSTOM_MORPHS = 1, SKYRIM_RACEMENU_UNAPPLIED_OVERRIDES = 2,
    SKYRIM_RACEMENU_UNAPPLIED_SKIN = 4, SKYRIM_RACEMENU_UNAPPLIED_BODY_MORPHS = 8,
    SKYRIM_RACEMENU_UNAPPLIED_TINTS = 16, SKYRIM_RACEMENU_UNAPPLIED_HEAD_PARTS = 32,
    SKYRIM_RACEMENU_UNAPPLIED_SCULPT = 64
};

typedef enum {
    SKYRIM_RACEMENU_PRESET_OK, SKYRIM_RACEMENU_PRESET_TOO_LARGE, SKYRIM_RACEMENU_PRESET_MALFORMED,
    SKYRIM_RACEMENU_PRESET_NO_FACE_MORPHS, SKYRIM_RACEMENU_PRESET_BAD_MORPHS,
    SKYRIM_RACEMENU_PRESET_BAD_PRESETS, SKYRIM_RACEMENU_PRESET_NO_MEMORY
} SkyrimRaceMenuPresetStatus;

typedef struct {
    float morphs[SKYRIM_RACEMENU_MORPHS];
    unsigned presets[SKYRIM_RACEMENU_PRESETS];
    unsigned unapplied; /* SKYRIM_RACEMENU_UNAPPLIED_* */
} SkyrimRaceMenuPreset;

/* Bounded: input size, nesting depth and node count. Outputs are written only on success. */
SkyrimRaceMenuPresetStatus skyrim_racemenu_preset_parse(const char *json, size_t length, SkyrimRaceMenuPreset *out);
#ifdef __cplusplus
}
#endif
