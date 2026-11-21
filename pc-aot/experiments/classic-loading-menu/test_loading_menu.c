/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"
#include "loading_menu.h"
#include "mod_loading_menu_host.h"

#include <assert.h>
#include <stdlib.h>

enum {
    MANAGER_SLOT = 0x012E3548, HOLDER_SLOT = 0x012E35E0,
    MANAGER = 0x12100, HOLDER = 0x12200, EVENT = 0x12300, STACK = 0x12FC0, LOADING_NAME = 0x12400
};

uint8_t *MEM;
uint64_t skyrim_perf_now_ns(void);
uint64_t skyrim_perf_now_ns(void) { return 1234567; }
static gm_space guest;
static uint32_t expected_event, expected_dispatcher, expected_return;
static unsigned dispatch_count, observations;
static ClassicLoadingMenuEvent observed;

void baseline_open(CPU *cpu);
void observed_open(CPU *cpu);
void baseline_close(CPU *cpu);
void observed_close(CPU *cpu);
void f_6EBC10(CPU *cpu);

static bool read_word(void *space, uint32_t address, uint32_t *value) {
    const void *bytes = gm_pointer(space, address, sizeof(*value));
    if (!bytes) return false;
    memcpy(value, bytes, sizeof(*value));
    return true;
}

void skyrim_loading_menu_observe(uint32_t site, uint32_t dispatcher, uint32_t event) {
    assert(dispatcher == expected_dispatcher && event == expected_event);
    observed = classic_loading_menu_event(site, dispatcher, event, read_word, &guest);
    ++observations;
}

void f_6EBC10(CPU *cpu) {
    assert(cpu->r[1] == expected_dispatcher && cpu->r[4] == STACK - 4);
    assert(M32(STACK - 4) == expected_return && M32(STACK) == expected_event);
    cpu->r[0] = 0x5a;
    cpu->r[4] += 8;
    ++dispatch_count;
}

static void run_case(bool opening, uint32_t dispatcher, uint32_t event, ClassicLoadingMenuEvent wanted) {
    CPU baseline = {0}, staged = {0};
    for (unsigned i = 0; i < 8; ++i) baseline.r[i] = 0x20 + i;
    baseline.r[1] = dispatcher;
    baseline.r[4] = STACK;
    memcpy(&staged, &baseline, sizeof(staged));
    M32(STACK) = expected_event = event;
    expected_dispatcher = dispatcher;
    expected_return = (opening ? CLASSIC_LOADING_MENU_OPEN_SITE : CLASSIC_LOADING_MENU_CLOSE_SITE) + 5;
    const uint32_t original_name = M32(EVENT), original_opening = M32(EVENT + 4);
    observations = dispatch_count = 0;
    (opening ? baseline_open : baseline_close)(&baseline);
    assert(dispatch_count == 1 && observations == 0);
    (opening ? observed_open : observed_close)(&staged);
    assert(dispatch_count == 2 && observations == 1 && observed == wanted);
    assert(!memcmp(staged.r, baseline.r, sizeof(staged.r)));
    assert(!memcmp(&staged.flags, &baseline.flags, sizeof(staged.flags)));
    assert(M32(EVENT) == original_name && M32(EVENT + 4) == original_opening);
}

int main(void) {
    assert(gm_reserve(&guest, NULL));
    assert(gm_commit_page(&guest, (uint64_t)MANAGER / guest.page_size * guest.page_size, NULL));
    assert(gm_commit_page(&guest, (uint64_t)MANAGER_SLOT / guest.page_size * guest.page_size, NULL));
    MEM = guest.base;
    M32(MANAGER_SLOT) = MANAGER;
    M32(HOLDER_SLOT) = HOLDER;
    M32(HOLDER + 0x74) = LOADING_NAME;
    M32(EVENT) = LOADING_NAME;
    M32(EVENT + 4) = 0xababab01;
    run_case(true, MANAGER + 4, EVENT, CLASSIC_LOADING_MENU_OPEN);
    skyrim_loading_menu_app_configure("different input");
    assert(!skyrim_loading_menu_app_ready());
    skyrim_loading_menu_app_observe(CLASSIC_LOADING_MENU_OPEN_SITE, MANAGER + 4, EVENT);
    skyrim_loading_menu_app_configure("d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605");
    assert(skyrim_loading_menu_app_ready());
    skyrim_loading_menu_app_observe(CLASSIC_LOADING_MENU_OPEN_SITE, MANAGER + 4, EVENT);
    skyrim_loading_menu_app_observe(CLASSIC_LOADING_MENU_OPEN_SITE, MANAGER + 4, 0x500);
    run_case(false, MANAGER + 4, EVENT, CLASSIC_LOADING_MENU_NONE);
    M32(EVENT + 4) = 0xababab00;
    run_case(false, MANAGER + 4, EVENT, CLASSIC_LOADING_MENU_CLOSE);
    skyrim_loading_menu_app_observe(CLASSIC_LOADING_MENU_CLOSE_SITE, MANAGER + 4, EVENT);
    skyrim_loading_menu_app_observe(CLASSIC_LOADING_MENU_CLOSE_SITE, MANAGER + 0x34, EVENT);
    run_case(false, MANAGER + 0x34, EVENT, CLASSIC_LOADING_MENU_NONE);
    run_case(false, MANAGER + 4, 0, CLASSIC_LOADING_MENU_NONE);
    run_case(false, MANAGER + 4, UINT32_MAX - 2, CLASSIC_LOADING_MENU_NONE);
    run_case(false, MANAGER + 4, 0x50000, CLASSIC_LOADING_MENU_NONE);
    M32(EVENT) = LOADING_NAME + 4;
    run_case(false, MANAGER + 4, EVENT, CLASSIC_LOADING_MENU_NONE);
    M32(EVENT) = LOADING_NAME;
    M32(HOLDER_SLOT) = 0;
    run_case(false, MANAGER + 4, EVENT, CLASSIC_LOADING_MENU_NONE);
    M32(HOLDER_SLOT) = HOLDER;
    M32(MANAGER_SLOT) = 0;
    run_case(false, MANAGER + 4, EVENT, CLASSIC_LOADING_MENU_NONE);
    assert(classic_loading_menu_event(0, MANAGER + 4, EVENT, read_word, &guest) == CLASSIC_LOADING_MENU_NONE);
    return gm_release(&guest, NULL) ? 0 : EXIT_FAILURE;
}
