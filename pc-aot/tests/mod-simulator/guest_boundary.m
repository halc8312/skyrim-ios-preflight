/* SPDX-License-Identifier: GPL-3.0-only
 * Only used by the simulator UIKit test app. No SDL window, Vulkan device,
 * guest loop, or shader cache exists in this process. */
#define SDL_MAIN_HANDLED
#import <Foundation/Foundation.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "device_policy.h"
#include "ios_runtime.h"
#include "lifecycle_runtime.h"
#include "pipeline_cache_control.h"
#include <stdio.h>
#include <stdlib.h>

static _Noreturn void unexpected(const char *function) {
    fprintf(stderr, "Unexpected call into an absent guest service: %s\n", function);
    abort();
}

SDL_bool SDLCALL SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info) {
    (void)window; (void)info; unexpected(__func__);
}
const char *SDLCALL SDL_GetError(void) { unexpected(__func__); }
int platform_text_input(int enabled) { (void)enabled; unexpected(__func__); }
void skyrim_ios_drawable_size(int *width, int *height) { (void)width; (void)height; unexpected(__func__); }
void skyrim_ios_metrics_active(int active) { (void)active; unexpected(__func__); }
void skyrim_ios_take_frame_metrics(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) {
    (void)a; (void)b; (void)c; (void)d; unexpected(__func__);
}
void skyrim_device_platform_refresh(void) { unexpected(__func__); }
void skyrim_device_platform_log(void) { unexpected(__func__); }
void skyrim_lifecycle_request(int active) { (void)active; unexpected(__func__); }
void skyrim_lifecycle_did_background(void) { unexpected(__func__); }
uint64_t skyrim_pipeline_checkpoint_begin(void) { unexpected(__func__); }
void skyrim_pipeline_checkpoint_cancel(uint64_t ticket) { (void)ticket; unexpected(__func__); }
void skyrim_pipeline_checkpoint_save(uint64_t ticket) { (void)ticket; unexpected(__func__); }
uint64_t skyrim_pipeline_checkpoint_foreground_begin(void) { unexpected(__func__); }
void skyrim_pipeline_checkpoint_foreground_permission(int allowed) {
    /* Showing Settings queries this permission. All shader trials are off. */
    if (allowed) unexpected(__func__);
}
