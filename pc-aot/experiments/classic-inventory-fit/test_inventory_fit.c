/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>

enum {
    STACK = 0x12800, MOVIE = 0x12100, VTABLE = 0x12200, MODE_METHOD = 0x5000,
    VARIABLE_METHOD, SET_ARGUMENT_METHOD, VISIBLE_METHOD, SAFE_METHOD, VIEWPORT_METHOD,
    DISPLAY = 0x12CF5B0, INVENTORY_NAME = 0x10E5B7C, MAGIC_NAME = 0x10E6584,
    LOCKPICKING_NAME = 0x10E62CC, QUEST_JOURNAL_NAME = 0x10EA0EC,
    LETTERBOX = 0x1BA75E1, WIDE = 0x1BA7408, SAFE_X = 0x12C33D8, SAFE_Y = 0x12C33E4,
    SAFE_X_WIDE = 0x12C33F0, SAFE_Y_WIDE = 0x12C33FC
};
uint8_t *MEM;
static bool enabled;
static unsigned mode, calls;
static float visible[4], safe[4];
static uint32_t viewport[6];

void baseline_fit(CPU *cpu);
void staged_fit(CPU *cpu);
void baseline_viewport_fit(CPU *cpu);
void staged_viewport_fit(CPU *cpu);
bool skyrim_inventory_fit_enabled(void) { return enabled; }

/* This fixture uses the ordinary non-letterboxed path with no SWF argument. */
void f_4096F0(CPU *c) { (void)c; assert(false); }
void f_402840(CPU *c) { (void)c; assert(false); }
void f_F5269C(CPU *c) { (void)c; assert(false); }

void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    (void)cache;
    static const uint32_t order[] = {
        MODE_METHOD, VARIABLE_METHOD, SET_ARGUMENT_METHOD, VISIBLE_METHOD, SAFE_METHOD, VIEWPORT_METHOD
    };
    assert(c->r[1] == MOVIE && calls < sizeof order / sizeof *order && target == order[calls]);
    ++calls;
    const uint32_t stack = c->r[4], argument = M32(stack + 4);
    switch (target) {
    case MODE_METHOD:
        assert(stack == STACK - 8 && M32(stack) == 0xA60D45);
        mode = argument;
        break;
    case VARIABLE_METHOD:
        c->r[0] = 0;
        break;
    case SET_ARGUMENT_METHOD:
        break;
    case VISIBLE_METHOD: {
        // Authored implementation of ShowAll and NoBorder for a 1280x720 stage.
        const double x_scale = M32(DISPLAY) / 1280.0, y_scale = M32(DISPLAY + 4) / 720.0;
        const double scale = mode == 1 ? fmin(x_scale, y_scale) : fmax(x_scale, y_scale);
        const double stage_width = M32(DISPLAY) / scale, stage_height = M32(DISPLAY + 4) / scale;
        visible[0] = (float)((1280 - stage_width) * 0.5);
        visible[1] = (float)((720 - stage_height) * 0.5);
        visible[2] = visible[0] + (float)stage_width;
        visible[3] = visible[1] + (float)stage_height;
        for (unsigned i = 0; i < 4; ++i) MF32(argument + 4 * i) = visible[i];
        break;
    }
    case SAFE_METHOD:
        for (unsigned i = 0; i < 4; ++i) safe[i] = MF32(argument + 4 * i);
        break;
    case VIEWPORT_METHOD:
        for (unsigned i = 0; i < 6; ++i) viewport[i] = M32(argument + 4 * i);
        assert(MF32(argument + 0x28) == 1.0f && MF32(argument + 0x2c) == 1.0f);
        break;
    }
    c->r[4] += target == VARIABLE_METHOD ? 12 : 8;
}

static CPU run(void (*entry)(CPU *), uint32_t kind, uint32_t requested, uint32_t width, uint32_t height,
               unsigned expected_calls) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.r[6] = MOVIE;
    M32(STACK + 0x178) = kind;
    M32(STACK + 0x17C) = requested;
    M32(DISPLAY) = width;
    M32(DISPLAY + 4) = height;
    calls = 0;
    entry(&cpu);
    assert(calls == expected_calls && cpu.r[4] == STACK && !cpu.top);
    assert(M32(STACK + 0x178) == kind && M32(STACK + 0x17C) == requested);
    return cpu;
}

static void compare(uint32_t kind, uint32_t requested, uint32_t width, uint32_t height, uint32_t selected) {
    const CPU original = run(baseline_fit, kind, requested, width, height, 1);
    assert(mode == requested);
    const CPU staged = run(staged_fit, kind, requested, width, height, 1);
    assert(mode == selected);
    for (unsigned i = 1; i < 8; ++i) assert(original.r[i] == staged.r[i]);
    if (selected == requested) assert(original.r[0] == staged.r[0]);
}

static void compare_viewport(uint32_t menu, uint32_t width, uint32_t height, bool changed) {
    const uint32_t dims[] = {width, height, 0, 0, width, height};
    run(baseline_viewport_fit, menu, 3, width, height, 6);
    assert(mode == 3 && !memcmp(viewport, dims, sizeof dims));
    if (changed) assert(visible[1] > 60 && visible[3] < 660 && safe[1] > 15);
    run(staged_viewport_fit, menu, 3, width, height, 6);
    assert(mode == (changed ? 1u : 3u) && !memcmp(viewport, dims, sizeof dims));
    assert(fabsf(safe[0] - 15.0f) < 0.01f && fabsf(safe[1] - 15.0f) < 0.01f);
    assert(fabsf(safe[2] - ((float)width - 15)) < 0.01f && fabsf(safe[3] - ((float)height - 15)) < 0.01f);
    assert(visible[1] == 0.0f && visible[3] == 720.0f);
}

int main(void) {
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    assert(gm_commit_page(&space, (uint64_t)STACK / space.page_size * space.page_size, NULL));
    assert(gm_commit_page(&space, (uint64_t)DISPLAY / space.page_size * space.page_size, NULL));
    MEM = space.base;
    M32(MOVIE) = VTABLE;
    M32(VTABLE + 0x6C) = MODE_METHOD;
    M32(VTABLE + 0x44) = VARIABLE_METHOD;
    M32(VTABLE + 0xAC) = SET_ARGUMENT_METHOD;
    M32(VTABLE + 0x7C) = VISIBLE_METHOD;
    M32(VTABLE + 0x8C) = SAFE_METHOD;
    M32(VTABLE + 0x64) = VIEWPORT_METHOD;
    const unsigned globals[] = {WIDE, LETTERBOX, SAFE_X};
    for (size_t i = 0; i < sizeof globals / sizeof *globals; ++i)
        assert(gm_commit_page(&space, globals[i] / space.page_size * space.page_size, NULL));
    MF32(SAFE_X) = MF32(SAFE_Y) = MF32(SAFE_X_WIDE) = MF32(SAFE_Y_WIDE) = 15.0f;
    static const uint32_t menus[] = {REVIEWED_MENU_POINTERS};
    for (size_t i = 0; i < sizeof menus / sizeof *menus; ++i)
        compare(menus[i], 3, 2868, 1320, 3);
    enabled = true;
    for (size_t i = 0; i < sizeof menus / sizeof *menus; ++i) {
        compare(menus[i], 3, 1920, 1080, 3);
        compare(menus[i], 3, 854, 480, 3); /* Rounded 16:9. */
        compare(menus[i], 3, 956, 440, 1);
        compare(menus[i], 3, 2868, 1320, 1);
    }
    compare(MAGIC_NAME, 3, 1440, 1080, 3);
    compare(INVENTORY_NAME, 3, 0, 440, 3);
    compare(INVENTORY_NAME, 3, UINT32_MAX, 440, 3);
    compare(MAGIC_NAME, 2, 956, 440, 2);
    compare(LOCKPICKING_NAME, 3, 956, 440, 3);
    compare(QUEST_JOURNAL_NAME, 1, 956, 440, 1);
    M8(WIDE) = 1;
    compare_viewport(INVENTORY_NAME, 1564, 720, true);
    compare_viewport(MAGIC_NAME, 1564, 720, true);
    M8(WIDE) = 0;
    compare_viewport(INVENTORY_NAME, 1280, 720, false);
    assert(gm_release(&space, NULL));
}
