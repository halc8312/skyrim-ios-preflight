/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"
#include "funcs_exe.h"
#include "lift_menu_seam.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    FS_BASE = 0x2000, TLS_VECTOR = 0x2050, TLS_DATA = 0x2100, TLS_TAG = TLS_DATA + 0x4ac,
    MENU = 0x2600, SELECTION = 0x2680, NODE = 0x2700, MARKED = 0x2800, OTHER = 0x2840, TOPIC = 0x2900,
    TITLE = 0x2a00, VOICE = 0x2b00, OTHER_VOICE = 0x2c00,
    UI_EVENT = 0x2d00, UI_PORT = 0x2e00, UI_NOTIFY_TABLE = 0x2f00,
    PAYLOAD = 0x3040, COPIED_TITLE = 0x3100, UI_MANAGER = 0x3400, QUEUE_COUNT = UI_MANAGER + 0x1c8,
    QUEUE_SLOT = UI_MANAGER + 0x1cc, STACK = 0x3d00,
    TLS_KEY = 0x01bbeb54, UI_PORT_SLOT = 0x012e35e0, UI_MANAGER_SLOT = 0x012e35e4,
    SUBTITLE_SETTING = 0x012e35ec, HEAP = 0x01b418b0, NOTIFY = 0x300,
    SELECTED_RETURN = 0x00675caa, OTHER_RETURN = 0x00675c06,
};

static const char line[] = "The gate remains closed until sunrise";
static const char voice_candidate[] = "Data\\Sound\\Voice\\Skyrim.esm\\Authored.wav";
static const char stock_path[] = "Sound/Voice/Fuz Ro Doh/Authored_4.xwm";
static const char assigned_stock[] = "Data\\Sound\\Voice\\Fuz Ro Doh\\Authored_4.xwm";

typedef struct {
    gm_space memory;
    SkyrimFuzLiftMemory access;
    SkyrimFuzMenuTracker tracker;
    SkyrimFuzPolicy policy;
    bool setting, original_present, stock_present, refuse_assignment, move_cursor_on_read, refuse_title_allocation;
    unsigned assignments, settings, selections, checks, records, title_copies, notifications;
    unsigned frees, voice_releases, menu_clears;
} Fixture;

uint8_t *MEM;
static Fixture fixture;

void skyrim_fuz_probe_voice_assignment(CPU *);
void skyrim_fuz_probe_selected_response(CPU *);
void skyrim_fuz_probe_original_selection(CPU *);

static void commit(uint32_t address) {
    size_t page_size = fixture.memory.page_size;
    uint64_t page = ((uint64_t)address / page_size) * page_size;
    if (!gm_pointer(&fixture.memory, address, 1)) assert(gm_commit_page(&fixture.memory, page, NULL));
}

static bool guest_read(void *context, uint32_t address, void *out, size_t size) {
    const void *data = gm_pointer(context, address, size);
    if (!data) return false;
    memcpy(out, data, size);
    return true;
}

static bool guest_replace(void *context, uint32_t address, const void *expected, const void *replacement, size_t size) {
    void *data = gm_pointer(context, address, size);
    if (!data || memcmp(data, expected, size)) return false;
    memcpy(data, replacement, size);
    return true;
}

static const char *guest_text(uint32_t address, size_t max) {
    const char *text = gm_pointer(&fixture.memory, address, max);
    assert(text && memchr(text, 0, max));
    return text;
}

static void copy_text(uint32_t address, const char *text) {
    size_t count = strlen(text) + 1;
    void *destination = gm_pointer(&fixture.memory, address, count);
    assert(destination);
    memcpy(destination, text, count);
}

static const char *asset_leaf(const char *path) {
    const char *leaf = strrchr(path, '/');
    return leaf ? leaf + 1 : path;
}

static SkyrimFuzAvailability scripted_resources(void *context, const char *path) {
    const Fixture *state = context;
    if (!strcmp(path, stock_path)) return state->stock_present ? SKYRIM_FUZ_PRESENT : SKYRIM_FUZ_ABSENT;
    assert(!strcmp(asset_leaf(path), "Authored.wav") || !strcmp(asset_leaf(path), "Authored.fuz") ||
           !strcmp(asset_leaf(path), "Authored.xwm"));
    return state->original_present ? SKYRIM_FUZ_PRESENT : SKYRIM_FUZ_ABSENT;
}

void skyrim_fuz_host_forget_response(uint32_t response) {
    skyrim_fuz_menu_forget(&fixture.tracker, response);
}

void skyrim_fuz_host_choose_menu_voice(uint32_t argument, uint32_t response, SkyrimFuzLiftChoice *choice) {
    (void)skyrim_fuz_apply_lifted_path_choice(&fixture.policy, &fixture.access, argument, response, choice);
}

void skyrim_fuz_host_note_menu_assignment(uint32_t response, const SkyrimFuzLiftChoice *choice) {
    (void)skyrim_fuz_menu_assigned(&fixture.tracker, &fixture.access, response, choice);
}

SkyrimFuzMenuScope skyrim_fuz_host_capture_menu(uint32_t manager, uint32_t node, uint32_t title) {
    ++fixture.selections;
    assert(manager == MENU && node == NODE && title == TITLE);
    return skyrim_fuz_menu_capture(&fixture.tracker, &fixture.access, manager, node, title);
}

bool skyrim_fuz_host_force_menu(const SkyrimFuzMenuScope *scope, uint32_t caller, uint32_t title) {
    ++fixture.checks;
    return skyrim_fuz_menu_force(&fixture.tracker, &fixture.access, scope, caller, title);
}

static uint32_t argument(CPU *cpu, unsigned index) {
    return M32(cpu->r[4] + 4 * (index + 1));
}

static void return_guest(CPU *cpu, unsigned popped, uint32_t value) {
    cpu->r[0] = value;
    cpu->r[4] += (popped + 1) * 4;
}

void f_A51210(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x0067326d && (cpu->r[1] == MARKED + 0x10 || cpu->r[1] == OTHER + 0x10));
    uint32_t frame = argument(cpu, 0);
    assert(frame == STACK + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT);
    const char *path = guest_text(frame, SKYRIM_FUZ_LIFT_FRAME_BYTES);
    uint32_t data = !strcmp(path, assigned_stock) ? VOICE : OTHER_VOICE;
    if (!fixture.refuse_assignment) {
        copy_text(data, path);
        M32(cpu->r[1]) = data;
    }
    ++fixture.assignments;
    return_guest(cpu, 1, cpu->r[1]);
}

void f_892710(CPU *cpu) {
    assert(cpu->r[1] == TOPIC && M8(TLS_TAG) == 0x43);
    ++fixture.settings;
    if (fixture.move_cursor_on_read) M32(NODE) = OTHER;
    return_guest(cpu, 0, fixture.setting);
}

void f_8982E0(CPU *cpu) {
    ++fixture.menu_clears;
    return_guest(cpu, 0, 0);
}

void f_547A00(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x898111 && argument(cpu, 0) == UI_PORT + 0x24);
    assert(fixture.notifications == 0);
    memset(MEM + UI_EVENT, 0, 16);
    ++fixture.records;
    return_guest(cpu, 0, UI_EVENT);
}

void f_A48D60(CPU *cpu) {
    assert(cpu->r[1] == HEAP && !argument(cpu, 1) && !argument(cpu, 2));
    if (M32(cpu->r[4]) == 0x898126) {
        assert(argument(cpu, 0) == 8 && fixture.records == 1);
        return_guest(cpu, 3, PAYLOAD);
        return;
    }
    assert(M32(cpu->r[4]) == 0x402c57 && argument(cpu, 0) == sizeof line);
    return_guest(cpu, 3, fixture.refuse_title_allocation ? 0 : COPIED_TITLE);
}

static void secure_copy(CPU *cpu) {
    const uint32_t target = argument(cpu, 0), capacity = argument(cpu, 1);
    const uint32_t source = argument(cpu, 2), count = argument(cpu, 3);
    assert(target == COPIED_TITLE && source == TITLE && capacity == sizeof line && count == sizeof line - 1);
    void *destination = gm_pointer(&fixture.memory, target, capacity);
    const void *input = gm_pointer(&fixture.memory, source, count);
    assert(destination && input);
    memmove(destination, input, count);
    ++fixture.title_copies;
    return_guest(cpu, 0, 0);
}

void f_F522BA(CPU *cpu) { secure_copy(cpu); }
void f_F52317(CPU *cpu) { secure_copy(cpu); }

void f_A51280(CPU *cpu) {
    assert((cpu->r[1] == UI_EVENT + 0xc && argument(cpu, 0) == UI_PORT + 0xec) ||
           (cpu->r[1] == QUEUE_SLOT && argument(cpu, 0) == UI_PORT + 0x4c));
    M32(cpu->r[1]) = M32(argument(cpu, 0));
    return_guest(cpu, 1, cpu->r[1]);
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(target == NOTIFY && cpu->r[1] == UI_MANAGER + 4 && M32(argument(cpu, 0)) == QUEUE_SLOT);
    assert(M32(QUEUE_SLOT + 8) == UI_EVENT && M8(QUEUE_SLOT + 0xc) == 1);
    assert(M32(UI_EVENT + 8) == PAYLOAD && fixture.notifications == 0);
    ++fixture.notifications;
    return_guest(cpu, 1, 1);
}

void f_431AA0(CPU *cpu) { (void)cpu; assert(!"The deferred overflow route is outside this fixture."); }

void f_6F48A0(CPU *cpu) {
    assert(cpu->r[1] == NODE);
    return_guest(cpu, 0, !M32(NODE));
}

void f_A511B0(CPU *cpu) {
    assert(cpu->r[1] == MARKED + 0x10 && fixture.tracker.count == 0);
    assert(M32(cpu->r[1]) == VOICE);
    ++fixture.voice_releases;
    M32(cpu->r[1]) = 0;
    return_guest(cpu, 0, 0);
}

void f_A487B0(CPU *cpu) {
    assert(cpu->r[1] == HEAP && !argument(cpu, 1));
    const uint32_t address = argument(cpu, 0);
    if (address == TITLE) memset(MEM + TITLE, 0, sizeof line);
    else {
        assert(address == MARKED && fixture.voice_releases == 1 && fixture.tracker.count == 0);
        memset(MEM + address, 0, 0x24);
    }
    ++fixture.frees;
    return_guest(cpu, 2, 0);
}

void f_48CB30(CPU *cpu) {
    assert(cpu->r[1] == NODE && !M32(NODE) && !M32(NODE + 4) && !argument(cpu, 0) && !argument(cpu, 1));
    return_guest(cpu, 2, 0);
}

static CPU with_stack(void) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.fs_base = FS_BASE;
    return cpu;
}

static void new_response(uint32_t response) {
    CPU cpu = with_stack();
    cpu.r[1] = response;
    f_673170(&cpu);
    assert(cpu.r[0] == response && cpu.r[4] == STACK + 4 && !M32(response + 0x10));
    copy_text(TITLE, line);
    M32(response) = TITLE;
    M16(response + 4) = sizeof line - 1;
    M16(response + 6) = sizeof line;
}

static void assign_voice(uint32_t response) {
    CPU cpu = with_stack();
    cpu.r[1] = response + 0x10;
    cpu.r[6] = response;
    uint32_t frame = STACK + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT;
    M32(STACK) = frame;
    memset(MEM + frame, 0, SKYRIM_FUZ_LIFT_FRAME_BYTES);
    copy_text(frame, voice_candidate);
    skyrim_fuz_probe_voice_assignment(&cpu);
    assert(cpu.r[4] == STACK + 4);
}

static void reset_queue(void) {
    fixture.settings = fixture.selections = fixture.checks = fixture.records = fixture.title_copies =
        fixture.notifications = fixture.menu_clears = 0;
    M32(QUEUE_COUNT) = 0;
    M32(TLS_TAG) = 0x10203040;
    memset(MEM + COPIED_TITLE, 0x55, sizeof line);
}

static void select_line(uint32_t response, void (*dispatch)(CPU *)) {
    M32(NODE) = response;
    CPU cpu = with_stack();
    cpu.r[3] = MENU;
    M8(STACK + 0x3c) = 0;
    dispatch(&cpu);
    assert(cpu.r[4] == STACK && M32(TLS_TAG) == 0x10203040);
}

static void check_queued(void) {
    assert(M32(QUEUE_COUNT) == 1 && fixture.records == 1 && fixture.notifications == 1 && fixture.title_copies == 1);
    assert(M32(QUEUE_SLOT + 8) == UI_EVENT && M32(UI_EVENT + 8) == PAYLOAD);
    assert(M32(PAYLOAD) == COPIED_TITLE && M16(PAYLOAD + 4) == sizeof line - 1);
    assert(!strcmp(guest_text(COPIED_TITLE, sizeof line), line));
}

static void test_menu_handoff(void) {
    new_response(MARKED);
    fixture.refuse_assignment = true;
    assign_voice(MARKED);
    assert(fixture.assignments == 1 && !fixture.tracker.count);
    fixture.refuse_assignment = false;
    assign_voice(MARKED);
    assert(fixture.assignments == 2 && fixture.tracker.count == 1);
    assert(M32(MARKED + 0x10) == VOICE && !strcmp(guest_text(VOICE, sizeof assigned_stock), assigned_stock));

    new_response(OTHER);
    fixture.original_present = true;
    assign_voice(OTHER);
    assert(fixture.tracker.count == 1 && !strcmp(guest_text(OTHER_VOICE, sizeof voice_candidate), voice_candidate));
    M32(OTHER + 0x10) = VOICE; // Authored collision has the same text pointer and stock string.
    reset_queue();
    select_line(OTHER, skyrim_fuz_probe_selected_response);
    assert(fixture.selections == 1 && fixture.checks == 1 && !fixture.records);
    reset_queue();
    select_line(MARKED, skyrim_fuz_probe_original_selection);
    assert(fixture.settings == 1 && !fixture.checks && !fixture.records);

    reset_queue();
    fixture.setting = true;
    select_line(OTHER, skyrim_fuz_probe_selected_response);
    check_queued();
    assert(!fixture.checks);
    fixture.setting = false;
    reset_queue();
    M16(TOPIC + 0x24) = 0x0200;
    select_line(OTHER, skyrim_fuz_probe_selected_response);
    check_queued();
    M16(TOPIC + 0x24) = 0;

    reset_queue();
    fixture.move_cursor_on_read = true;
    select_line(MARKED, skyrim_fuz_probe_selected_response);
    assert(!fixture.records && fixture.checks == 1);
    fixture.move_cursor_on_read = false;

    reset_queue();
    M32(NODE) = MARKED;
    SkyrimFuzMenuScope old = skyrim_fuz_menu_capture(&fixture.tracker, &fixture.access, MENU, NODE, TITLE);
    CPU cpu = with_stack();
    M32(STACK) = OTHER_RETURN;
    M32(STACK + 4) = TITLE;
    f_8980D0(&cpu); // Actor-text caller cannot inherit the marked response.
    assert(cpu.r[4] == STACK + 4 && !fixture.checks && !fixture.records);
    cpu = with_stack();
    skyrim_fuz_host_queue_selected(&cpu, &old);
    assert(!fixture.records && fixture.checks == 1);

    reset_queue();
    M16(MARKED + 6) = 0;
    select_line(MARKED, skyrim_fuz_probe_selected_response);
    assert(!fixture.records && fixture.checks == 1);
    M16(MARKED + 6) = sizeof line;

    reset_queue();
    fixture.refuse_title_allocation = true;
    select_line(MARKED, skyrim_fuz_probe_selected_response);
    assert(fixture.notifications == 1 && !fixture.title_copies && !M32(PAYLOAD));
    fixture.refuse_title_allocation = false;

    reset_queue();
    select_line(MARKED, skyrim_fuz_probe_selected_response);
    check_queued();
    assert(fixture.selections == 1 && fixture.checks == 1);
    cpu = with_stack();
    cpu.r[1] = NODE;
    f_672EA0(&cpu);
    assert(cpu.r[4] == STACK + 4 && !M32(NODE) && fixture.frees == 2 && fixture.voice_releases == 1);
    assert(!M8(TITLE) && !fixture.tracker.count);
    check_queued(); // Original queue owns a text copy after the response dies.

    new_response(MARKED);
    M32(MARKED + 0x10) = VOICE; // An ordinary reused address with otherwise identical data.
    M32(NODE) = MARKED;
    assert(!skyrim_fuz_menu_force(&fixture.tracker, &fixture.access, &old, SELECTED_RETURN, TITLE));
    reset_queue();
    select_line(MARKED, skyrim_fuz_probe_selected_response);
    assert(!fixture.records && fixture.checks == 1);
}

int main(void) {
    assert(gm_reserve(&fixture.memory, NULL));
    MEM = fixture.memory.base;
    const uint32_t inputs[] = {FS_BASE, STACK, TLS_KEY, UI_PORT_SLOT};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) commit(inputs[i]);
    M32(FS_BASE + 0x2c) = TLS_VECTOR;
    M32(TLS_VECTOR) = TLS_DATA;
    M32(TLS_KEY) = 0;
    M32(UI_PORT_SLOT) = UI_PORT;
    M32(UI_MANAGER_SLOT) = UI_MANAGER;
    M32(SUBTITLE_SETTING) = TOPIC;
    M32(UI_MANAGER + 4) = UI_NOTIFY_TABLE;
    M32(UI_NOTIFY_TABLE + 4) = NOTIFY;
    M32(MENU + 0x1c) = SELECTION;
    M32(SELECTION + 0x20) = NODE;
    M32(SELECTION + 0x18) = TOPIC;
    M32(NODE + 4) = 0;
    fixture.access = (SkyrimFuzLiftMemory){guest_read, guest_replace, &fixture.memory};
    fixture.policy = (SkyrimFuzPolicy){
        .lookup = scripted_resources, .context = &fixture, .words_per_second = 2, .skip_empty_responses = true,
    };
    fixture.policy.stock_paths[3] = stock_path;
    fixture.stock_present = true;

    test_menu_handoff();
    assert(gm_release(&fixture.memory, NULL));
    puts("Selected-response subtitle gate kept independent identity and queued an owned text copy.");
    return 0;
}
