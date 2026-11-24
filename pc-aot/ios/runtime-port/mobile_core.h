/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} SkyrimMobilePad;
/* Control IDs also key saved touch positions: keep Move at 18. */
enum {
    SKYRIM_PAD_LT = 16, SKYRIM_PAD_RT = 17, SKYRIM_PAD_MOVE = 18,
    SKYRIM_PAD_BOTH_HANDS = 19, SKYRIM_PAD_CONTROL_COUNT = 20
};
uint64_t skyrim_mobile_now(void);
void skyrim_mobile_landscape_size(int *width, int *height);
void skyrim_mobile_button(unsigned index, int down);
void skyrim_mobile_button_cancel(unsigned index);
void skyrim_mobile_move(float x, float y, float deadzone);
void skyrim_mobile_look(float dx, float dy, float seconds, float height,
                        float sensitivity, int invert);
void skyrim_mobile_look_end(void);
void skyrim_mobile_clear(void);
SkyrimMobilePad skyrim_mobile_pad(void);
void skyrim_mobile_quicksave_key(void);
int skyrim_mobile_f5_down(void);
void skyrim_mobile_set_limit(unsigned fps, int thermal_guard);
void skyrim_mobile_set_thermal(unsigned state);
unsigned skyrim_mobile_fps_limit(void);
/* One presentation owner uses a local clock. Late frames discard backlog. */
typedef struct { uint64_t next; unsigned fps; } SkyrimFrameClock;
uint64_t skyrim_mobile_frame_deadline(SkyrimFrameClock *clock, uint64_t now, unsigned fps);
/* Pure input functions are also used by the authored regression test. */
void skyrim_mobile_stick(float x, float y, float deadzone, int16_t *ox, int16_t *oy);
void skyrim_mobile_camera(float dx, float dy, float seconds, float height,
                           float sensitivity, int invert, int16_t *ox, int16_t *oy);
/* Fade a stationary finger's last velocity, without changing the deadzone.
 * A fresh sample is unchanged; a lift/cancel still clears immediately. */
void skyrim_mobile_camera_age(int16_t x, int16_t y, uint64_t age_ns,
                              uint64_t sample_ns, int16_t *ox, int16_t *oy);
#ifdef __cplusplus
}
#endif
