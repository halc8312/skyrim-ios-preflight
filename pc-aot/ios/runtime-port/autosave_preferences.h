/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* A startup-only override for the four existing SkyrimPrefs autosave options.
 * -1 (or any unsupported value) leaves the current preferences alone. This
 * does not alter manual/F5 saves, the timer, graphics, or running guest state. */
static inline int skyrim_autosave_ini(double mode, unsigned index,
                                      const char **section, const char **key,
                                      const char **value) {
    static const char *const keys[] = {
        "bSaveOnPause", "bSaveOnTravel", "bSaveOnWait", "bSaveOnRest"
    };
    if ((mode != 0 && mode != 1) || index >= sizeof keys / sizeof keys[0])
        return 0;
    *section = "MAIN";
    *key = keys[index];
    *value = mode == 1 ? "1" : "0";
    return 1;
}
