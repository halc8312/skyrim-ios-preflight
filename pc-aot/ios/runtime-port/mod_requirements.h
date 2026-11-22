/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Read-only check of what the SkyUI 5.1 trial needs from the selected game
 * folders: SkyUI's plugin and Classic BSA, and the owner's compiled SKSE 1.7.3
 * Papyrus scripts. Never writes, never follows links. */
typedef enum {
    SKYRIM_SKYUI_REQ_OK,
    SKYRIM_SKYUI_REQ_SKYUI_ABSENT,           /* no SkyUI.esp in any root */
    SKYRIM_SKYUI_REQ_SKYUI_ARCHIVE_UNSUPPORTED, /* SkyUI.bsa missing or not a Classic (104) archive */
    SKYRIM_SKYUI_REQ_SKSE_SCRIPT_MISSING,    /* Scripts/SKSE.pex is absent */
    SKYRIM_SKYUI_REQ_SKSE_SCRIPT_INVALID,    /* unreadable, oversized or without SKSE.GetVersionRelease */
    SKYRIM_SKYUI_REQ_UNREADABLE              /* a link, an ambiguous alias or an I/O error */
} SkyrimSkyUIRequirement;

enum { SKYRIM_SKYUI_SUPPORT_SCRIPTS = 4 }; /* UI, UICallback, Input, StringUtil */

typedef struct {
    SkyrimSkyUIRequirement status;
    unsigned support_present;  /* of SKYRIM_SKYUI_SUPPORT_SCRIPTS */
    unsigned support_expected;
} SkyrimSkyUIRequirements;

/* data_roots: the original Data folder, then enabled packages' Data folders in
 * overwrite order (later wins). Returns 0, or EINVAL for bad arguments. */
int skyrim_skyui_requirements(const char *const *data_roots, size_t count,
                              SkyrimSkyUIRequirements *out);
enum { SKYRIM_XPMSE_FNIS_HKX = 3, SKYRIM_XPMSE_FNIS_LISTS = 2 };

typedef struct {
    unsigned fnis_hkx32;    /* known 32-bit headers among the common behavior files */
    unsigned fnis_hkx64;    /* known incompatible 64-bit headers */
    unsigned fnis_hkx_unknown; /* present with an unrecognized header */
    unsigned fnis_lists;    /* installed animation data text files */
    unsigned fnis_plugin;   /* FNIS.esp present, selection is checked separately */
    unsigned skse_script;   /* verified SKSE.GetVersionRelease PEX declaration */
    unsigned nioverride;    /* supplied by RaceMenu or a standalone package */
    unsigned unreadable;    /* ambiguous paths, links or read errors */
} SkyrimXpmseRequirements;

/* Inspect ordered Data roots. These checks describe the available files. The
 * generator configuration and in-game functionality require separate checks. */
int skyrim_xpmse_requirements(const char *const *data_roots, size_t count,
                              SkyrimXpmseRequirements *out);
#ifdef __cplusplus
}
#endif
