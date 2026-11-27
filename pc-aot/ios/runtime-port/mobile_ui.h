/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "shader_options.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Startup settings/INI values become immutable before guest threads start. */
void skyrim_mobile_initialize(const char *profile);
int skyrim_mobile_wants_arabic_game_text(void);
/* Once after initialization and before guest threads. Only app-owned profile
 * preferences are written; original game archives/masters are never changed. */
int skyrim_mobile_prepare_game_language(const char *game_directory, unsigned arabic_mod);
/* Whether the prepared game profile can use the verified Arabic text overlay. */
int skyrim_mobile_uses_arabic_game_text(void);
#if defined(SKYRIM_IOS_CLASSIC_SKSE) || defined(SKYDREAM_SKSE)
/* Effective text from an explicitly prepared game profile, or NULL when
 * the guest's existing ini supplies the language. Immutable during play. */
const char *skyrim_mobile_classic_text_language(void);
int skyrim_mobile_classic_game_language(unsigned *text, unsigned *voices);
#endif
/* Once after the language step and before guest threads. Reads only plugin
 * headers in Data and publishes the profile plugins.txt for the user's MOD
 * choices. Returns 0 if a vanilla launch could not restore the profile's
 * original list. Such a launch must not use a stale managed list. */
int skyrim_mobile_prepare_mods(const char *game_directory);
int skyrim_mobile_verbose(void);
int skyrim_mobile_shader_option(unsigned option);
void skyrim_mobile_install(void *sdl_window);
void skyrim_mobile_tick(void);
void skyrim_mobile_frame(void);
void skyrim_mobile_render_size(int *width, int *height);
int skyrim_mobile_render_override(int *width, int *height);
int skyrim_mobile_graphics_changed(void);
int skyrim_mobile_ini(unsigned index, const char **section, const char **key, const char **value);
#ifdef __cplusplus
}
#endif
