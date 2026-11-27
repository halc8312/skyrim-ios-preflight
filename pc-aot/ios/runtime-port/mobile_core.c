/* SPDX-License-Identifier: GPL-3.0-only */
#include "mobile_core.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SkyrimMobilePad pad;
static uint32_t held;
static uint64_t tap_until[SKYRIM_PAD_CONTROL_COUNT], look_time, look_interval, f5_until;
static unsigned limit, thermal;
static int guard;

void skyrim_mobile_landscape_size(int *width, int *height) {
    if (*width > 0 && *width < *height) {
        int short_edge = *width;
        *width = *height;
        *height = short_edge;
    }
}

int skyrim_mobile_choose_render_size(int width, int height, int requested,
                                    int *render_width, int *render_height) {
    if (width < 2 || height < 2 || width > 16384 || height > 16384 || requested < 2)
        return 0;
    skyrim_mobile_landscape_size(&width, &height);
    int target = (requested == 2160 || requested > height ? height : requested) & ~1;
    int scaled = (int)(((int64_t)width * target + height / 2) / height) & ~1;
    *render_width = scaled;
    *render_height = target;
    return 1;
}

SkyrimMobileViewport skyrim_mobile_viewport(int drawable_width, int drawable_height,
                                           unsigned render_width, unsigned render_height) {
    if (drawable_width <= 0 || drawable_height <= 0) return (SkyrimMobileViewport){0};
    SkyrimMobileViewport full = {0, 0, drawable_width, drawable_height};
    if (!render_width || !render_height) return full;
    int64_t width = (int64_t)drawable_height * render_width / render_height;
    int height = drawable_height;
    if (width > drawable_width) {
        width = drawable_width;
        height = (int)((int64_t)drawable_width * render_height / render_width);
    }
    if (drawable_width - width <= 2 && drawable_height - height <= 2) return full;
    return (SkyrimMobileViewport){(int)((drawable_width - width) / 2),
                                  (drawable_height - height) / 2, (int)width, height};
}

int skyrim_mobile_viewport_point(SkyrimMobileViewport viewport, int drawable_width,
                                int drawable_height, double window_x, double window_y,
                                double *game_x, double *game_y) {
    if (drawable_width <= 0 || drawable_height <= 0 || viewport.width <= 0 || viewport.height <= 0) return 0;
    double x = (window_x * drawable_width - viewport.x) / viewport.width;
    double y = (window_y * drawable_height - viewport.y) / viewport.height;
    if (!(x >= 0 && x < 1 && y >= 0 && y < 1)) return 0;
    if (game_x) *game_x = x;
    if (game_y) *game_y = y;
    return 1;
}

uint64_t skyrim_mobile_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static float bounded(float x, float a, float b) {
    return isfinite(x) ? fminf(b, fmaxf(a, x)) : a;
}
void skyrim_mobile_stick(float x, float y, float deadzone, int16_t *ox, int16_t *oy) {
    *ox = *oy = 0;
    if (!isfinite(x) || !isfinite(y)) return;
    float length = hypotf(x, y);
    float d = bounded(deadzone, 0, 0.4f);
    if (length <= d || length < 0.0001f) return;
    float amount = fminf(1, (length - d) / (1 - d));
    *ox = (int16_t)lroundf(x / length * amount * 32767);
    *oy = (int16_t)lroundf(y / length * amount * 32767);
}
void skyrim_mobile_camera(float dx, float dy, float seconds, float height,
                         float sensitivity, int invert, int16_t *ox, int16_t *oy) {
    *ox = *oy = 0;
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(seconds) ||
        !isfinite(height) || seconds <= 0 || seconds > 0.5f || height < 1 || height > 100000) return;
    dx = bounded(dx, -height*4, height*4); dy = bounded(dy, -height*4, height*4);
    float scale = bounded(sensitivity, 0.25f, 4) / (height * fmaxf(seconds, 0.004f));
    float x = dx * scale, y = dy * scale * (invert ? 1 : -1);
    float length = hypotf(x, y);
    if (length < 0.003f) return;
    /* Compensate the game's right-stick deadzone without modifying the game.
     * Finger velocity is normalized to screen height, independent of Retina. */
    float amount = 0.27f + 0.73f * fminf(length, 1);
    *ox = (int16_t)lroundf(x / length * amount * 32767);
    *oy = (int16_t)lroundf(y / length * amount * 32767);
}
void skyrim_mobile_button(unsigned index, int down) {
    if (index >= SKYRIM_PAD_CONTROL_COUNT || index == SKYRIM_PAD_MOVE) return;
    pthread_mutex_lock(&lock);
    if (down) {
        held |= 1u << index;
        tap_until[index] = skyrim_mobile_now() + UINT64_C(120000000);
    } else held &= ~(1u << index);
    pthread_mutex_unlock(&lock);
}
void skyrim_mobile_button_cancel(unsigned index) {
    if (index >= SKYRIM_PAD_CONTROL_COUNT || index == SKYRIM_PAD_MOVE) return;
    pthread_mutex_lock(&lock);
    held &= ~(1u << index);
    tap_until[index] = 0;
    pthread_mutex_unlock(&lock);
}
int skyrim_mobile_menu_swipe(float dx, float dy) {
    if (!isfinite(dx) || !isfinite(dy)) return -1;
    const float x = fabsf(dx), y = fabsf(dy);
    if (y >= 28 && y >= 1.5f * x) return dy < 0 ? SKYRIM_PAD_DOWN : SKYRIM_PAD_UP;
    if (x >= 44 && x >= 1.5f * y) return dx < 0 ? SKYRIM_PAD_RB : SKYRIM_PAD_LB;
    return -1;
}
int skyrim_mobile_menu_rect(unsigned control, float width, float height, SkyrimMenuButtonRect *out) {
    /* Signed row numbers keep both rails centered and leave the first/last list entries visible. */
    static const int8_t rows[] = {
        [SKYRIM_MENU_SETTINGS]=1, [SKYRIM_PAD_LB]=2, [SKYRIM_PAD_UP]=3,
        [SKYRIM_PAD_DOWN]=4, [SKYRIM_PAD_LEFT]=5, [SKYRIM_PAD_RIGHT]=6,
        [SKYRIM_MENU_EXPAND]=-1, [SKYRIM_PAD_RB]=-2, [SKYRIM_PAD_A]=-3,
        [SKYRIM_PAD_B]=-4, [SKYRIM_PAD_LT]=-5, [SKYRIM_PAD_RT]=-6
    };
    if (!out || control >= sizeof(rows) / sizeof(rows[0]) || !rows[control] ||
        !isfinite(width) || !isfinite(height) || width < 4 || height < 6) return 0;
    const float gap=fminf(3, height/18);
    const float side=fminf(44, fminf((height-5*gap)/6, width/2));
    int row=rows[control];
    out->side=side;
    out->x=row < 0 ? width-side : 0;
    unsigned index=(unsigned)(row < 0 ? -row : row) - 1;
    out->y=(height-6*side-5*gap)/2 + index*(side+gap);
    return 1;
}
void skyrim_mobile_button_pulse(unsigned index) {
    if (index >= SKYRIM_PAD_CONTROL_COUNT || index == SKYRIM_PAD_MOVE) return;
    pthread_mutex_lock(&lock);
    /* A swipe can coexist with a different finger holding this same direction. */
    tap_until[index] = skyrim_mobile_now() + UINT64_C(120000000);
    pthread_mutex_unlock(&lock);
}
void skyrim_mobile_camera_age(int16_t x, int16_t y, uint64_t age_ns,
                             uint64_t sample_ns, int16_t *ox, int16_t *oy) {
    /* UIKit may batch events. Keep one event interval plus 8 ms of jitter
     * allowance, then fade the velocity over one interval. Bound that interval
     * to 8--34 ms so 30 Hz delivery stays steady without extending a stale
     * turn indefinitely after a slow frame. */
    uint64_t interval = sample_ns < UINT64_C(8000000) ? UINT64_C(8000000) : sample_ns;
    if (interval > UINT64_C(34000000)) interval = UINT64_C(34000000);
    const uint64_t hold = interval + UINT64_C(8000000);
    if (age_ns <= hold) { *ox=x; *oy=y; return; }
    if (age_ns >= hold + interval) { *ox=*oy=0; return; }
    float length=hypotf((float)x,(float)y);
    if (length < 1) { *ox=*oy=0; return; }
    /* Fade the velocity above the compensated deadzone, not the entire stick
     * vector: scaling that vector would cross the game's deadzone abruptly. */
    float gain=(float)(hold+interval-age_ns)/(float)interval;
    float velocity=fmaxf(0,(length/32767.0f-.27f)/.73f);
    float amount=fminf(length,(.27f+.73f*velocity*gain)*32767.0f);
    *ox=(int16_t)lroundf((float)x/length*amount);
    *oy=(int16_t)lroundf((float)y/length*amount);
}
void skyrim_mobile_move(float x, float y, float deadzone) {
    int16_t ax, ay;
    skyrim_mobile_stick(x, y, deadzone, &ax, &ay);
    pthread_mutex_lock(&lock);
    pad.lx = ax; pad.ly = ay;
    pthread_mutex_unlock(&lock);
}
void skyrim_mobile_look(float dx, float dy, float seconds, float height, float sensitivity, int invert) {
    int16_t ax, ay;
    skyrim_mobile_camera(dx, dy, seconds, height, sensitivity, invert, &ax, &ay);
    pthread_mutex_lock(&lock);
    pad.rx = ax; pad.ry = ay;
    look_time = skyrim_mobile_now();
    look_interval = isfinite(seconds) && seconds > 0 && seconds <= .5f
        ? (uint64_t)((double)seconds * 1e9) : 0;
    pthread_mutex_unlock(&lock);
}
void skyrim_mobile_look_end(void) {
    pthread_mutex_lock(&lock);
    pad.rx = pad.ry = 0; look_time = look_interval = 0;
    pthread_mutex_unlock(&lock);
}
void skyrim_mobile_clear(void) {
    pthread_mutex_lock(&lock);
    memset(&pad, 0, sizeof pad); memset(tap_until, 0, sizeof tap_until);
    held = 0; look_time = look_interval = f5_until = 0;
    pthread_mutex_unlock(&lock);
}
SkyrimMobilePad skyrim_mobile_pad(void) {
    uint64_t now = skyrim_mobile_now();
    pthread_mutex_lock(&lock);
    SkyrimMobilePad result = pad;
    uint32_t buttons = held;
    for (unsigned i = 0; i < SKYRIM_PAD_CONTROL_COUNT; ++i)
        if (tap_until[i] > now) buttons |= 1u << i;
    /* One source drives both triggers in the same snapshot. Releasing it must
     * not clear a separately held LT/RT (or the physical controller downstream).
     * Short taps keep the same 120 ms minimum pulse as the individual controls. */
    if (buttons & (1u << SKYRIM_PAD_BOTH_HANDS))
        buttons |= (1u << SKYRIM_PAD_LT) | (1u << SKYRIM_PAD_RT);
    result.buttons = (uint16_t)buttons;
    result.lt = buttons & (1u << SKYRIM_PAD_LT) ? 255 : 0;
    result.rt = buttons & (1u << SKYRIM_PAD_RT) ? 255 : 0;
    if (!look_time || now < look_time) result.rx = result.ry = 0;
    else skyrim_mobile_camera_age(result.rx, result.ry, now-look_time,
                                  look_interval, &result.rx, &result.ry);
    pthread_mutex_unlock(&lock);
    return result;
}
void skyrim_mobile_quicksave_key(void) {
    pthread_mutex_lock(&lock);
    f5_until = skyrim_mobile_now() + UINT64_C(500000000);
    pthread_mutex_unlock(&lock);
}
int skyrim_mobile_f5_down(void) {
    uint64_t now = skyrim_mobile_now();
    pthread_mutex_lock(&lock);
    int down = f5_until > now;
    pthread_mutex_unlock(&lock);
    return down;
}
void skyrim_mobile_set_limit(unsigned fps, int thermal_guard) {
    if (fps != 0 && fps != 24 && fps != 30 && fps != 40 && fps != 60) fps = 0;
    pthread_mutex_lock(&lock);
    limit = fps; guard = !!thermal_guard;
    pthread_mutex_unlock(&lock);
}
void skyrim_mobile_set_thermal(unsigned state) {
    pthread_mutex_lock(&lock); thermal = state > 3 ? 3 : state; pthread_mutex_unlock(&lock);
}
unsigned skyrim_mobile_fps_limit(void) {
    pthread_mutex_lock(&lock);
    unsigned fps = limit;
    if (guard && thermal >= 2) {
        unsigned cap = thermal == 3 ? 24 : 30;
        if (!fps || fps > cap) fps = cap;
    }
    pthread_mutex_unlock(&lock);
    return fps;
}
uint64_t skyrim_mobile_frame_deadline(SkyrimFrameClock *clock, uint64_t now, unsigned fps) {
    if (!fps || fps > 240) { clock->next = 0; clock->fps = 0; return now; }
    uint64_t period = UINT64_C(1000000000) / fps;
    if (clock->fps != fps || !clock->next || now >= clock->next || clock->next - now > period * 2) {
        clock->fps = fps; clock->next = now + period; return now;
    }
    uint64_t deadline = clock->next;
    clock->next = deadline + period;
    return deadline;
}
