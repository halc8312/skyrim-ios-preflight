/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t profile[16];
    int settings_fd, game_fd;
    uint8_t *dll, *pex;
    size_t dll_size, pex_size;
    uint8_t *wearable_plugin, *storage_pex;
    size_t wearable_plugin_size, storage_pex_size;
} SkyrimPapyrusInputs;

/* Open the app's private settings directory and snapshot one paired package
 * from an already finalized view. The caller supplies an open read-only app
 * support descriptor, the running named profile UUID and the frozen enabled
 * package UUIDs in deployment order. The function does not take support_fd.
 * Returns errno, leaving both output descriptors at -1 on error. */
int skyrim_papyrus_inputs_open(int support_fd, const char *profile_id,
                             const char *const *package_ids, size_t count,
                             SkyrimPapyrusInputs *out);
/* Safe after any acquisition result and after an earlier release. */
void skyrim_mods_release_papyrus_inputs(SkyrimPapyrusInputs *inputs);
