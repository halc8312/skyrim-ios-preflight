/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int skyrim_ios_runtime_main(int argc, char **argv);
void skyrim_ios_stage(const char *stage);
void skyrim_ios_present_result(int32_t result);
void skyrim_ios_set_window(void *window);
void skyrim_ios_pump_events(void);
void skyrim_ios_drawable_size(int *width, int *height);
void skyrim_ios_present_phase(unsigned phase);
void skyrim_ios_watchdog(void);
#ifdef __cplusplus
}
#endif
