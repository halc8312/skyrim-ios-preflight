/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_SKYUI_DENSITY_H
#define SKYRIM_SKYUI_DENSITY_H

#include <stdbool.h>
#include <stddef.h>

enum SkyrimSkyUIDensity { SKYUI_STANDARD, SKYUI_COMPACT, SKYUI_LARGE_TEXT };
enum SkyrimSkyUIMessage { SKYUI_IGNORE, SKYUI_OVERRIDE_KEYS, SKYUI_OVERRIDE_VALUES };
enum { SKYUI_OVERRIDE_COUNT = 128, SKYUI_DENSITY_MAX_EXTRAS = 16 };

typedef struct {
    const char *key;
    const char *value;
} SkyrimSkyUIOverride;

/* Set the verified effective preference before the guest enters. */
void skyrim_skyui_density_configure(enum SkyrimSkyUIDensity mode);
enum SkyrimSkyUIDensity skyrim_skyui_density_mode(void);
enum SkyrimSkyUIMessage skyrim_skyui_density_message(const char *target, size_t count);

/* The caller must check that the live movie has the corresponding key suffix
 * and is at LOAD_FILE before forwarding any extra values. */
size_t skyrim_skyui_density_plan(enum SkyrimSkyUIDensity mode, const char *menu,
    SkyrimSkyUIOverride out[SKYUI_DENSITY_MAX_EXTRAS]);

#endif
