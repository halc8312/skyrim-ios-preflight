/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"
#include "mobile_core.h"
#include "menu_focus.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STACK = 0x15700, CURSOR = 0x12100, INPUT = 0x12180,
    TLS = 0x12200, TLS_TABLE = 0x12280, TLS_DATA = 0x12400,
    HOLDER = 0x12ae0, POOL = 0x13000, QUEUE = 0x14000, WRAPPER = 0x14020,
    MESSAGE = 0x14040, MANAGER = 0x14100, MENU_STACK = 0x14200,
    CURSOR_MENU = 0x14300, INVENTORY = 0x14340, MAGIC = 0x14380, MODAL = 0x143c0,
    CURSOR_VIEW = 0x14400, INVENTORY_VIEW = 0x14420, MAGIC_VIEW = 0x14440,
    MOVIE_VTABLE = 0x14500, MODAL_VTABLE = 0x14600, MENU_STATE = 0x14700,
    HEAP_BLOCK = 0x14800, INPUT_DISPATCHER = 0x16000, INPUT_BUFFER = 0x16100, INPUT_MANAGER = 0x16400,
    CURSOR_VTABLE = 0x10e4bd4, INVENTORY_VTABLE = 0x10e5b90, MAGIC_VTABLE = 0x10e6594,
    CURSOR_NAME = 0x110df94, HANDLE_EVENT = 0x01000100, HANDLE_MODAL = 0x01000200,
    SET_VARIABLE = 0x01000300, ADVANCE_MOVIE = 0x01000400
};
static gm_space guest;
uint8_t *MEM;
static unsigned queued;
static float expected_x, expected_y;
static uint32_t deliveries[4];
static unsigned delivery_count;
static CPU *tick_cpu;
static unsigned tick_dispatches;
static ClassicMenuFocus focus = CLASSIC_MENU_FOCUS_INIT;
static _Thread_local CPU *thread_cpu;
static uint64_t now_ns;
static unsigned delivered_focus, advances;
static bool heap_in_use;

void f_A62020(CPU *cpu);
void f_859A70(CPU *cpu);
void f_86BA80(CPU *cpu);
void f_875E40(CPU *cpu);
void f_A68F20(CPU *cpu);
void f_A64980(CPU *cpu);
void baseline_advance(CPU *cpu);
void fixture_route(CPU *cpu);

static void assert_move(uint32_t event) {
    assert(M32(event) == 1);
    assert(MF32(event + 4) == expected_x && MF32(event + 8) == expected_y);
    assert(M32(event + 12) == 0 && M32(event + 16) == 0 && M32(event + 20) == 0);
}

void rt_call(CPU *cpu, uint32_t target) {
    assert(heap_in_use && cpu == thread_cpu);
    if (target == 0x86ba80) { assert(cpu->r[1] == INVENTORY); f_86BA80(cpu); return; }
    if (target == 0x875e40) { assert(cpu->r[1] == MAGIC); f_875E40(cpu); return; }
    abort();
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    switch (target) {
    case 0x859a70: assert(cpu->r[1] == CURSOR_MENU); f_859A70(cpu); return;
    case 0x86ba80: assert(cpu->r[1] == INVENTORY); f_86BA80(cpu); return;
    case 0x875e40: assert(cpu->r[1] == MAGIC); f_875E40(cpu); return;
    case HANDLE_EVENT:
        assert_move(M32(cpu->r[4] + 4));
        assert(cpu->r[1] == CURSOR_VIEW || cpu->r[1] == INVENTORY_VIEW || cpu->r[1] == MAGIC_VIEW);
        assert(delivery_count < sizeof(deliveries) / sizeof(deliveries[0]));
        deliveries[delivery_count++] = cpu->r[1];
        cpu->r[0] = 0;
        cpu->r[4] += 8;
        return;
    case HANDLE_MODAL:
        assert(cpu->r[1] == MODAL && M32(cpu->r[4] + 4) == MESSAGE);
        cpu->r[0] = 1;
        cpu->r[4] += 8;
        return;
    case SET_VARIABLE:
        assert(cpu->r[1] == INVENTORY_VIEW || cpu->r[1] == MAGIC_VIEW);
        cpu->r[0] = 1;
        cpu->r[4] += 16;
        return;
    case ADVANCE_MOVIE:
        assert(cpu->r[1] == INVENTORY_VIEW || cpu->r[1] == MAGIC_VIEW);
        ++advances;
        fpush(cpu, 0.0);
        cpu->r[4] += 12;
        return;
    default: abort();
    }
}

uint32_t gmalloc(uint32_t bytes) {
    assert(!heap_in_use && bytes == 52);
    heap_in_use = true;
    return HEAP_BLOCK;
}

void gfree(uint32_t block) {
    assert(heap_in_use && block == HEAP_BLOCK);
    heap_in_use = false;
    memset(MEM + HEAP_BLOCK, 0xda, 52);
}

void skyrim_menu_focus_advancing(CPU *cpu, unsigned menu, unsigned view) {
    CPU saved = *cpu;
    if (classic_menu_focus_advance(&focus, cpu, thread_cpu, menu, view, now_ns)) ++delivered_focus;
    assert(!memcmp(&saved, cpu, sizeof saved) && !heap_in_use);
}

void (*skyrim_menu_focus_callback)(CPU *, unsigned, unsigned) = skyrim_menu_focus_advancing;

void f_547A00(CPU *cpu) {
    assert(M32(cpu->r[4] + 4) == HOLDER + 0x20);
    cpu->r[0] = WRAPPER;
    cpu->r[4] += 4;
}

void f_431B00(CPU *cpu) {
    uint32_t name = M32(cpu->r[4] + 4);
    assert(cpu->r[1] == QUEUE && name == HOLDER + 0x5c);
    assert(M32(name) == CURSOR_NAME && strcmp((const char *)MEM + CURSOR_NAME, "Cursor Menu") == 0);
    assert(M32(cpu->r[4] + 8) == 6 && M32(cpu->r[4] + 12) == WRAPPER);
    assert(M32(WRAPPER + 8) == POOL + 0x2f0 + queued * 24);
    assert_move(M32(WRAPPER + 8));
    assert(M32(TLS_DATA + 0x4ac) == 0x43);
    M32(MESSAGE) = M32(name);
    M32(MESSAGE + 4) = 6;
    M32(MESSAGE + 8) = WRAPPER;
    ++queued;
    cpu->r[4] += 16;
}

void f_401710(CPU *cpu) {
    assert(cpu->r[1] == 0x1b8d084 && M32(cpu->r[4] + 4) == 0);
    ++M32(0x1b8d088);
    cpu->r[4] += 8;
}

void f_A66EC0(CPU *cpu) {
    assert(cpu == tick_cpu && cpu->r[1] == INPUT_MANAGER);
    assert(M32(cpu->r[4] + 4) == 0);
    cpu->r[4] += 8;
}

void f_A66400(CPU *cpu) {
    assert(cpu == tick_cpu);
    cpu->r[4] += 4;
}

void f_6EBC10(CPU *cpu) {
    assert(cpu == tick_cpu && cpu->r[1] == INPUT_DISPATCHER);
    assert(M32(cpu->r[4]) == 0xa68f8b);
    uint32_t event_slot = M32(cpu->r[4] + 4);
    assert(event_slot == STACK + 4 && M32(event_slot) == 0);
    ++tick_dispatches;
    cpu->r[4] += 8;
}

void f_A698A0(CPU *cpu) {
    assert(cpu == tick_cpu && cpu->r[1] == INPUT_BUFFER && tick_dispatches == 1);
    cpu->r[4] += 4;
}

static void commit(uint32_t address) {
    assert(gm_commit_page(&guest, (uint64_t)address / guest.page_size * guest.page_size, NULL));
}

/* Only the private fixture reads the validated original's switch and vtable entries. */
static void load_tables(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    uint32_t address, size;
    unsigned loaded = 0;
    while (fread(&address, sizeof(address), 1, file) == 1) {
        assert(fread(&size, sizeof(size), 1, file) == 1 && size && size <= 64);
        commit(address);
        commit(address + size - 1);
        assert(fread(MEM + address, 1, size, file) == size);
        ++loaded;
    }
    assert(feof(file) && loaded == 6 && fclose(file) == 0);
}

static void route(const uint32_t *stack, size_t count) {
    for (size_t i = 0; i < count; ++i) M32(MENU_STACK + 4 * i) = stack[i];
    M32(MANAGER + 0x94) = MENU_STACK;
    M32(MANAGER + 0x9c) = (uint32_t)count;
    M32(STACK + 0x30) = MANAGER + 0x94;
    CPU cpu = {0};
    cpu.r[3] = CURSOR_MENU;
    cpu.r[4] = STACK;
    cpu.r[5] = MESSAGE;
    cpu.r[6] = MANAGER;
    cpu.fs_base = TLS;
    delivery_count = 0;
    fixture_route(&cpu);
    assert(cpu.r[4] == STACK && cpu.top == 0 && M32(0x1b8d088) == 1);
    assert(M32(TLS_DATA + 0x4ac) == 0x55);
}

static void point(SkyrimMobileViewport viewport, double window_x, double window_y) {
    double game_x, game_y;
    assert(skyrim_mobile_viewport_point(viewport, 2868, 1320, window_x, window_y, &game_x, &game_y));
    expected_x = (float)(game_x * 1920);
    expected_y = (float)(game_y * 1080);
    assert(expected_x >= 0 && expected_x < 1920 && expected_y >= 0 && expected_y < 1080);
    MF32(CURSOR + 4) = expected_x;
    MF32(CURSOR + 8) = expected_y;
    CPU cpu = {0};
    cpu.r[1] = CURSOR;
    cpu.r[4] = STACK;
    cpu.fs_base = TLS;
    M32(STACK + 4) = INPUT;
    f_A62020(&cpu);
    assert(cpu.r[4] == STACK + 8 && cpu.top == 0);
    assert(M32(TLS_DATA + 0x4ac) == 0x55);
    assert(M32(INPUT + 0x18) == 0 && M32(INPUT + 0x1c) == 0);
}

static void advance(CPU *cpu, uint32_t menu, bool baseline) {
    *cpu = (CPU){0};
    cpu->r[1] = menu;
    cpu->r[4] = STACK;
    cpu->fs_base = TLS;
    cpu->r[3] = 0x445566;
    M32(STACK + 4) = 0;
    M32(STACK + 8) = 1;
    if (baseline) baseline_advance(cpu);
    else f_A64980(cpu);
    assert(cpu->r[4] == STACK + 12 && cpu->top == 0 && !heap_in_use);
}

static void *wrong_native_thread(void *opaque) {
    CPU *cpu = opaque;
    assert(!classic_menu_focus_advance(&focus, cpu, cpu, INVENTORY, INVENTORY_VIEW, now_ns));
    return NULL;
}

static void test_immediate_focus(void) {
    M32(0x1b2e974) = CURSOR;
    M32(HOLDER + 0x44) = 0x10e5b7c;
    M32(HOLDER + 0x64) = 0x10e6584;
    M32(MOVIE_VTABLE + 0x40) = SET_VARIABLE;
    M32(MOVIE_VTABLE + 0x94) = ADVANCE_MOVIE;
    CPU cpu, expected;
    thread_cpu = &cpu;
    delivery_count = 0;
    advance(&expected, INVENTORY, true);
    advance(&cpu, INVENTORY, false);
    assert(!memcmp(&expected, &cpu, sizeof cpu) && !classic_menu_focus_token(&focus));
    classic_menu_focus_transition(&focus, CLASSIC_MENU_INVENTORY, &cpu);
    assert(!classic_menu_focus_token(&focus));
    advance(&cpu, INVENTORY, false);
    uint64_t token = classic_menu_focus_token(&focus);
    assert(token && !classic_menu_focus_post(&focus, token, NAN, .5, now_ns));
    assert(!classic_menu_focus_post(&focus, token, 1, .5, now_ns));
    expected_x = 960; expected_y = 540;
    assert(classic_menu_focus_post(&focus, token, .5, .5, now_ns));
    thread_cpu = &expected;
    advance(&cpu, INVENTORY, false);
    thread_cpu = &cpu;
    pthread_t other;
    assert(!pthread_create(&other, NULL, wrong_native_thread, &cpu));
    assert(!pthread_join(other, NULL));
    advance(&cpu, MAGIC, false);
    assert(!delivery_count && !delivered_focus);
    advance(&cpu, INVENTORY, false);
    assert(!memcmp(&expected, &cpu, sizeof cpu));
    assert(delivered_focus == 1 && delivery_count == 1 && deliveries[0] == INVENTORY_VIEW);
    advance(&cpu, INVENTORY, false);
    assert(delivered_focus == 1);
    assert(classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    now_ns += 250000001;
    advance(&cpu, INVENTORY, false);
    assert(delivered_focus == 1);
    assert(classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    classic_menu_focus_transition(&focus, CLASSIC_MENU_NONE, &cpu);
    classic_menu_focus_transition(&focus, CLASSIC_MENU_INVENTORY, &cpu);
    advance(&cpu, INVENTORY, false);
    assert(delivered_focus == 1 && !classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    token = classic_menu_focus_token(&focus);
    assert(classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    classic_menu_focus_transition(&focus, CLASSIC_MENU_MAGIC, &cpu);
    advance(&cpu, MAGIC, false);
    assert(delivered_focus == 1 && !classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    token = classic_menu_focus_token(&focus);
    assert(classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    M32(MAGIC + 8) = INVENTORY_VIEW;    /* Replacement of a movie without an intervening close event. */
    advance(&cpu, MAGIC, false);
    M32(MAGIC + 8) = MAGIC_VIEW;
    advance(&cpu, MAGIC, false);
    assert(delivered_focus == 1);
    expected_x = 480; expected_y = 270;
    token = classic_menu_focus_token(&focus);
    assert(classic_menu_focus_post(&focus, token, .25, .25, now_ns));
    advance(&cpu, MAGIC, false);
    assert(delivered_focus == 2 && delivery_count == 2 && deliveries[1] == MAGIC_VIEW);
    assert(queued == 2 && advances > 0);
    thread_cpu = NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(gm_reserve(&guest, NULL));
    MEM = guest.base;
    for (uint32_t address = 0x12000; address <= STACK; address += 0x1000) commit(address);
    load_tables(argv[1]);
    commit(0x12c36cc); commit(0x12e35e0); commit(0x1bbeb54); commit(0x1b3fcac);
    commit(0x1b2e99c); commit(0x1b8d088);
    M32(TLS + 0x2c) = TLS_TABLE;
    M32(TLS_TABLE) = TLS_DATA;
    M32(TLS_DATA + 0x4ac) = 0x55;
    M32(0x12e35e0) = HOLDER;
    M32(HOLDER + 0x5c) = CURSOR_NAME;
    M32(0x12e35e4) = QUEUE;
    M32(0x1b3fcac) = POOL;
    M32(0x1b2e99c) = MENU_STATE;
    M32(0x1b8d088) = 1;
    M32(CURSOR_MENU) = CURSOR_VTABLE; M32(CURSOR_MENU + 8) = CURSOR_VIEW;
    M32(INVENTORY) = INVENTORY_VTABLE; M32(INVENTORY + 8) = INVENTORY_VIEW;
    M32(MAGIC) = MAGIC_VTABLE; M32(MAGIC + 8) = MAGIC_VIEW;
    M32(CURSOR_VIEW) = M32(INVENTORY_VIEW) = M32(MAGIC_VIEW) = MOVIE_VTABLE;
    M32(MOVIE_VTABLE + 0xb4) = HANDLE_EVENT;
    M32(MODAL) = MODAL_VTABLE; M32(MODAL_VTABLE + 0x10) = HANDLE_MODAL;
    MF32(CURSOR + 0x14) = 1920;
    MF32(CURSOR + 0x18) = 1080;
    SkyrimMobileViewport viewport = skyrim_mobile_viewport(2868, 1320, 1920, 1080);
    assert(!skyrim_mobile_viewport_point(viewport, 2868, 1320, .03, .5, NULL, NULL));
    point(viewport, .5, .5);
    route((uint32_t[]){INVENTORY, CURSOR_MENU}, 2);
    assert(delivery_count == 2 && deliveries[0] == CURSOR_VIEW && deliveries[1] == INVENTORY_VIEW);
    point(viewport, .25, .2);
    route((uint32_t[]){MAGIC, CURSOR_MENU}, 2);
    assert(delivery_count == 2 && deliveries[0] == CURSOR_VIEW && deliveries[1] == MAGIC_VIEW);
    assert(queued == 2 && M32(POOL + 0x0c) == 2);
    /* The same queued hover follows whichever menu is open at delivery time. */
    route((uint32_t[]){INVENTORY, CURSOR_MENU}, 2);
    assert(delivery_count == 2 && deliveries[1] == INVENTORY_VIEW);
    route((uint32_t[]){MAGIC, MODAL, CURSOR_MENU}, 3);
    assert(delivery_count == 1 && deliveries[0] == CURSOR_VIEW);
    route((uint32_t[]){MAGIC}, 1);
    assert(delivery_count == 1 && deliveries[0] == CURSOR_VIEW);
    test_immediate_focus();
    assert(gm_release(&guest, NULL));

    /* A Classic input tick still dispatches on its existing CPU with no pending hardware event. */
    assert(gm_reserve(&guest, NULL));
    MEM = guest.base;
    commit(STACK); commit(INPUT_DISPATCHER); commit(0x1b40dbc); commit(0x12e7458);
    M32(0x1b40dbc) = INPUT_BUFFER;
    M32(0x12e7458) = INPUT_MANAGER;
    CPU cpu = {0};
    cpu.r[1] = INPUT_DISPATCHER;
    cpu.r[4] = STACK;
    M32(STACK + 4) = 0;
    tick_cpu = &cpu;
    f_A68F20(&cpu);
    assert(tick_dispatches == 1 && cpu.r[4] == STACK + 8);
    tick_cpu = NULL;
    assert(gm_release(&guest, NULL));
}
