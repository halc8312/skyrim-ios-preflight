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
void skyrim_ios_pace_frame(void);
void skyrim_ios_take_frame_metrics(uint64_t *present_count, uint64_t *present_ns,
                                  uint64_t *present_max_ns, uint64_t *interval_max_ns);
void skyrim_ios_watchdog(void);
#ifdef __cplusplus
}
#endif
