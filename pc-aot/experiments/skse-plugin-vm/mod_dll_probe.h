/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_MOD_DLL_PROBE_H
#define SKYRIM_EXPERIMENT_MOD_DLL_PROBE_H
#import <UIKit/UIKit.h>
#include <stdint.h>

UIViewController *skyrim_mod_dll_probe_controller(BOOL japanese, NSURL *initialFolder);

/* Set once at startup with the verified original game path and digest.
 * The view is loaded and checked again only for an explicit DLL diagnosis. */
void skyrim_mod_dll_probe_set_game_input(NSURL *input, const uint8_t digest[32], const uint8_t *bootstrap);
#endif
