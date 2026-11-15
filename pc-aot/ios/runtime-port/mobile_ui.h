/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "shader_options.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Startup settings/INI values become immutable before guest threads start. */
void skyrim_mobile_initialize(const char *profile);
/* Once after initialization and before guest threads. Only app-owned profile
 * preferences are written; original game archives/masters are never changed. */
int skyrim_mobile_prepare_game_language(const char *game_directory);
int skyrim_mobile_verbose(void);
int skyrim_mobile_shader_option(unsigned option);
void skyrim_mobile_install(void *sdl_window);
void skyrim_mobile_tick(void);
void skyrim_mobile_frame(void);
void skyrim_mobile_render_size(int *width, int *height);
int skyrim_mobile_graphics_changed(void);
int skyrim_mobile_ini(unsigned index, const char **section, const char **key, const char **value);
#ifdef __cplusplus
}
#endif
