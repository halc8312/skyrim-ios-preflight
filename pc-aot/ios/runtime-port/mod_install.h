/* SPDX-License-Identifier: GPL-3.0-only
 * MOD installer (FOMOD) screens and the in-app download browser. */
#pragma once
#ifdef __OBJC__
#import <UIKit/UIKit.h>

/* Run a FOMOD installer on the main thread. mod_name is shown at the top so
 * the user knows which MOD the choices belong to. raw is the fully extracted
 * archive; config is fomod/ModuleConfig.xml inside it; the chosen files are
 * copied into data (the package's staging Data folder). active holds
 * lower-case plugin names treated as installed and active for conditions.
 * completion(YES) after copying, completion(NO) when cancelled or invalid
 * (message explains). */
void skyrim_fomod_run(NSString *mod_name, NSString *raw, NSString *config, NSString *data, NSSet<NSString *> *active,
                      void (^completion)(BOOL installed, NSString *message));

/* Nexus Mods (2011 edition) in a web view; manual downloads are imported. */
UIViewController *skyrim_mods_browser_controller(void);
#endif
