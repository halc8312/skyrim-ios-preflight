/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>

enum {
    STACK = 0x12800, MOVIE = 0x12100, VTABLE = 0x12200, MODE_METHOD = 0x5000,
    DISPLAY = 0x12CF5B0, INVENTORY_NAME = 0x10E5B7C, MAGIC_NAME = 0x10E6584,
    LOCKPICKING_NAME = 0x10E62CC, QUEST_JOURNAL_NAME = 0x10EA0EC
};
uint8_t *MEM;
static bool enabled;
static unsigned mode, calls;

void baseline_fit(CPU *cpu);
void staged_fit(CPU *cpu);
bool skyrim_inventory_fit_enabled(void) { return enabled; }

void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(target == MODE_METHOD && c->r[1] == MOVIE && c->r[4] == STACK - 8);
    assert(M32(c->r[4]) == 0xA60D45 && M32(VTABLE + 0x6C) == MODE_METHOD);
    mode = M32(c->r[4] + 4);
    ++calls;
    c->r[4] += 8;
}

static CPU run(void (*entry)(CPU *), uint32_t kind, uint32_t requested, uint32_t width, uint32_t height) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.r[6] = MOVIE;
    M32(STACK + 0x178) = kind;
    M32(STACK + 0x17C) = requested;
    M32(DISPLAY) = width;
    M32(DISPLAY + 4) = height;
    calls = 0;
    entry(&cpu);
    assert(calls == 1 && cpu.r[4] == STACK);
    assert(M32(STACK + 0x178) == kind && M32(STACK + 0x17C) == requested);
    return cpu;
}

static void compare(uint32_t kind, uint32_t requested, uint32_t width, uint32_t height, uint32_t selected) {
    const CPU original = run(baseline_fit, kind, requested, width, height);
    assert(mode == requested);
    const CPU staged = run(staged_fit, kind, requested, width, height);
    assert(mode == selected);
    for (unsigned i = 1; i < 8; ++i) assert(original.r[i] == staged.r[i]);
    if (selected == requested) assert(original.r[0] == staged.r[0]);
}

int main(void) {
    gm_space space = {0};
    assert(gm_reserve(&space, NULL));
    assert(gm_commit_page(&space, (uint64_t)STACK / space.page_size * space.page_size, NULL));
    assert(gm_commit_page(&space, (uint64_t)DISPLAY / space.page_size * space.page_size, NULL));
    MEM = space.base;
    M32(MOVIE) = VTABLE;
    M32(VTABLE + 0x6C) = MODE_METHOD;
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
    assert(gm_release(&space, NULL));
}
