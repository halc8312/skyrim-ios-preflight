/* SPDX-License-Identifier: GPL-3.0-only
 * MOD packages, profiles, launch view, safe mode and sharing. Main thread
 * unless noted. Original game files and existing saves are never changed. */
#pragma once
#include <stdint.h>
#ifdef __OBJC__
#import <UIKit/UIKit.h>

/* Startup, in this order, before guest threads:
 * 1. select_profile: chooses this launch's profile (may ask the safe-mode
 *    question first) and returns its runtime profile directory.
 * 2. prepare_game: returns the folder the runtime should use as the game
 *    folder — the original when no MOD package is on, otherwise a clone with
 *    the packages laid over Data. Never fatal; falls back to the original. */
NSString *skyrim_mods_select_profile(NSString *default_profile_directory);
NSString *skyrim_mods_prepare_game(NSString *game_directory);
/* Text for the overlay after startup (nil when nothing to say). */
NSString *skyrim_mods_launch_notice(void);
/* Plugin on/off and order for the running profile (array of {name, on}). */
NSArray<NSDictionary *> *skyrim_mods_plugin_choices(void);
/* Frozen effective choices for this launch, accounting for disabled packages
 * and a safe-mode or failed-deployment fallback. */
BOOL skyrim_mods_manages_plugins(void);
NSArray<NSDictionary *> *skyrim_mods_launch_plugin_choices(void);
void skyrim_mods_no_plugin_loaded(void);
void skyrim_mods_set_plugin_choices(NSArray *rows);
/* Called with the running frame count; marks the launch healthy once. */
void skyrim_mods_frame(uint64_t frames);
/* Settings > MOD. */
UIViewController *skyrim_mods_hub_controller(void);
/* Accept a file shared to the app (zip / 7z / rar / MOD list). Any thread. */
void skyrim_mods_import_url(NSURL *url, BOOL security_scoped);

/* Warning when the newest save of the running profile lists plugins that are
 * not loaded now (nil otherwise). Call after the plugin list is prepared. */
NSString *skyrim_mods_save_notice(void);

/* Provided by mobile_ui.m. */
NSArray<NSString *> *skyrim_mobile_loaded_plugins(void);
void skyrim_mobile_show_notice(NSString *text);
UIViewController *skyrim_mobile_plugin_controller(void);
#endif
