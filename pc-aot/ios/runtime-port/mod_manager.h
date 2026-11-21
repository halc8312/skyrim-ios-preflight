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
#if defined(SKYRIM_IOS_CLASSIC_SKSE) && defined(SKYRIM_IOS_SKSE_DLL_PROBE)
#include "mod_papyrus_inputs.h"
/* On the startup main thread after skyrim_mobile_prepare_mods succeeds.
 * Eligible only for the opted-in running named profile and its finalized
 * MOD view. The caller releases the snapshots and descriptors. */
int skyrim_mods_acquire_papyrus_inputs(SkyrimPapyrusInputs *inputs);
#endif
#ifdef SKYRIM_IOS_CLASSIC_SKSE
/* 1 when the feature (a SkyrimFeature value) was chosen in the running profile
 * and passed its launch checks. Fixed after startup; any thread. */
int skyrim_mods_feature_enabled(unsigned feature);
/* Final numeric MOD feature diagnostics, after the guest returns (Classic builds). */
void skyrim_mods_report_features_at_exit(void);
#endif
/* Text for the overlay after startup (nil when nothing to say). */
NSString *skyrim_mods_launch_notice(void);
/* Plugin on/off and order for the running profile (array of {name, on}). */
NSArray<NSDictionary *> *skyrim_mods_plugin_choices(void);
/* Frozen effective choices for this launch, accounting for disabled packages
 * and a safe-mode or failed-deployment fallback. */
BOOL skyrim_mods_manages_plugins(void);
NSArray<NSDictionary *> *skyrim_mods_launch_plugin_choices(void);
void skyrim_mods_plugin_list_failed(void);
void skyrim_mods_no_plugin_loaded(void);
void skyrim_mods_set_plugin_choices(NSArray *rows);
#ifdef SKYRIM_IOS_CLASSIC_SKSE
/* Apply the selected profile's trial once, after guest binding and plugin
 * publication, before any guest code runs. */
BOOL skyrim_mods_prepare_skyui_trial(BOOL skyui_listed);
#endif
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
/* Pause the game (same safe path as an inactive app) while a MOD screen or the
 * installer is open, so UIKit has the main thread. Each reason is a flag. */
enum { SKYRIM_MENU_PAUSE_MODS = 1, SKYRIM_MENU_PAUSE_INSTALLER = 2 };
void skyrim_mobile_menu_pause(unsigned reason, int on);
NSArray<NSString *> *skyrim_mobile_loaded_plugins(void);
void skyrim_mobile_show_notice(NSString *text);
UIViewController *skyrim_mobile_plugin_controller(void);
#endif
