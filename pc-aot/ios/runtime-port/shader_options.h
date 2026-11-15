/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Host-only startup preferences. Existing game/guest ABI and graphics choices
 * are independent. Missing, malformed and non-1 values are disabled. */
enum SkyrimShaderOption {
    SKYRIM_SHADER_PARALLEL,
    SKYRIM_SHADER_PRIORITY,
    SKYRIM_SHADER_SAVE_IN_SETTINGS,
    SKYRIM_SHADER_PERSISTENT,
    SKYRIM_SHADER_REUSE,
    SKYRIM_SHADER_OPTION_COUNT
};
static inline const char *skyrim_shader_option_key(unsigned option) {
    static const char *const keys[] = {"shaderParallel", "shaderPriority",
        "shaderSaveInSettings", "shaderPersistent", "shaderReuse"};
    return option < SKYRIM_SHADER_OPTION_COUNT ? keys[option] : 0;
}
static inline const char *skyrim_shader_option_environment(unsigned option) {
    static const char *const names[] = {"SKYRIM_MVK_MODULE_COLD", "SKYRIM_PREFETCH_GATE",
        "SKYRIM_CACHE_FOREGROUND", "SKYRIM_CACHE_PERSISTENT", "SKYRIM_CACHE_REUSE"};
    return option < SKYRIM_SHADER_OPTION_COUNT ? names[option] : 0;
}
static inline int skyrim_shader_option_enabled(double value) { return value == 1.0; }
