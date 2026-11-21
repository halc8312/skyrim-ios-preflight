/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"
#include "funcs_exe.h"
#include "lift_world_seam.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

enum {
    FS_BASE = 0x2000, TLS_VECTOR = 0x2050, TLS_DATA = 0x2100, TLS_TAG = TLS_DATA + 0x4ac,
    MARKED = 0x2600, ORDINARY = 0x2640, TITLE = 0x2700, STOCK = 0x2780, REAL_VOICE = 0x2880,
    ACTOR = 0x2980, AUDIO_DEFINITION = 0x2a00, AUDIO_MANAGER = 0x2b80, AUDIO_TASK = 0x2be0,
    QUEUE = 0x2c00, ARRAY = QUEUE + 4, ROWS = 0x2d00, ROW_BYTES = SKYRIM_FUZ_WORLD_RECORD_BYTES,
    TEMP_TITLE = 0x2e00, OWNED_TITLES = 0x2e80, TITLE_STRIDE = 0x80, MAX_OWNED = 6,
    NODE = 0x3180, SWAP = 0x3200, GROWN_ROWS = 0x3300, STACK = 0x3d00,
    TLS_KEY = 0x1bbeb54, GENERAL_SETTING = 0x12b44bc, DIALOGUE_SETTING = 0x12b44c8,
    DISTANCE_SETTING = 0x12b44d4, QUEUE_SLOT = 0x12e35ec, INVALID_HANDLE = 0x1310630, PLAYER = 0x1b2e8e4,
    HEAP = 0x1b418b0, ACTOR_HANDLE = 0x000042aa, ARRAY_OPERATIONS = 0x1077640,
    INITIAL_CAPACITY = 4,
};
enum { SETTINGS_OFF, SHOW_GENERAL, OUT_OF_RANGE, REPEATED_SPEAKER };

static const uint32_t none_handle = UINT32_MAX;
static const char title[] = "The smith has finished the last order";
static const char wav_path[] = "Data\\Sound\\Voice\\Skyrim.esm\\Authored.wav";
static const char stock_resource[] = "Sound/Voice/Fuz Ro Doh/Authored_4.xwm";
static const char stock_path[] = "Data\\Sound\\Voice\\Fuz Ro Doh\\Authored_4.xwm";

typedef struct {
    gm_space space;
    SkyrimFuzLiftMemory memory;
    SkyrimFuzPolicy policy;
    SkyrimFuzMenuTracker responses;
    SkyrimFuzWorldTracker chatter;
    bool temporary, owned[MAX_OWNED], fail_copy, reject_title, original_voice, change_voice_after_submit;
    bool replace_response_during_submit, fail_growth, initial_rows_live, grown_rows_live;
    unsigned voice_submissions, records_copied, temp_releases, title_releases, response_releases;
    unsigned array_releases;
} Fixture;

uint8_t *MEM;
static Fixture host;

void skyrim_fuz_probe_voice_assignment(CPU *);
void skyrim_fuz_probe_world_source(CPU *);
void skyrim_fuz_probe_original_world_display(CPU *);
void skyrim_fuz_probe_world_display(CPU *);
void skyrim_fuz_probe_world_release_892E50(CPU *);
void skyrim_fuz_probe_world_release_893070(CPU *);

static void commit(uint32_t address) {
    if (gm_pointer(&host.space, address, 1)) return;
    const size_t size = host.space.page_size;
    assert(gm_commit_page(&host.space, (uint64_t)address / size * size, NULL));
}

static bool read_guest(void *context, uint32_t address, void *data, size_t size) {
    const void *source = gm_pointer(context, address, size);
    if (!source) return false;
    memcpy(data, source, size);
    return true;
}

static bool replace_guest(void *context, uint32_t address, const void *expected, const void *data, size_t size) {
    void *target = gm_pointer(context, address, size);
    if (!target || memcmp(target, expected, size)) return false;
    memcpy(target, data, size);
    return true;
}

static void in_guest(uint32_t address, const char *text) {
    size_t size = strlen(text) + 1;
    void *p = gm_pointer(&host.space, address, size);
    assert(p);
    memcpy(p, text, size);
}

static bool title_equals(uint32_t address, const char *wanted) {
    size_t length = strlen(wanted) + 1;
    const char *bytes = gm_pointer(&host.space, address, length);
    return bytes && !memcmp(bytes, wanted, length);
}

static SkyrimFuzAvailability find_resource(void *context, const char *path) {
    if (!strcmp(path, stock_resource)) return SKYRIM_FUZ_PRESENT;
    const Fixture *state = context;
    const char *name = strrchr(path, '/');
    assert(name && (!strcmp(name, "/Authored.wav") || !strcmp(name, "/Authored.fuz") || !strcmp(name, "/Authored.xwm")));
    return state->original_voice ? SKYRIM_FUZ_PRESENT : SKYRIM_FUZ_ABSENT;
}

void skyrim_fuz_host_forget_response(uint32_t response) {
    skyrim_fuz_menu_forget(&host.responses, response);
}

void skyrim_fuz_host_choose_menu_voice(uint32_t argument, uint32_t response, SkyrimFuzLiftChoice *choice) {
    (void)skyrim_fuz_apply_lifted_path_choice(&host.policy, &host.memory, argument, response, choice);
}

void skyrim_fuz_host_note_menu_assignment(uint32_t response, const SkyrimFuzLiftChoice *choice) {
    (void)skyrim_fuz_menu_assigned(&host.responses, &host.memory, response, choice);
}

SkyrimFuzWorldScope skyrim_fuz_host_capture_world(uint32_t response, uint32_t voice) {
    assert((response == MARKED || response == ORDINARY) && (voice == STOCK || voice == REAL_VOICE));
    return skyrim_fuz_world_capture(&host.responses, &host.memory, response, voice);
}

bool skyrim_fuz_host_admit_world(const SkyrimFuzWorldScope *scope, uint32_t caller, uint32_t source) {
    return skyrim_fuz_world_admit(&host.responses, &host.memory, scope, caller, source);
}

void skyrim_fuz_host_world_after_copy(uint32_t array, uint32_t index, uint32_t source, const SkyrimFuzWorldScope *scope) {
    assert(array == ARRAY && index < M32(ARRAY + 8) && host.temporary);
    assert(M32(source + 4) == TEMP_TITLE && (scope == NULL || scope->serial));
    skyrim_fuz_world_after_copy(&host.chatter, &host.responses, &host.memory, array, index, source, scope);
    ++host.records_copied;
}

void skyrim_fuz_host_release_chatter_title(uint32_t pointer) {
    skyrim_fuz_world_release_title(&host.chatter, pointer);
}

bool skyrim_fuz_host_world_general(uint32_t array, uint32_t record) {
    return skyrim_fuz_world_general(&host.chatter, &host.memory, array, record);
}

static uint32_t arg(CPU *cpu, unsigned n) {
    return M32(cpu->r[4] + 4 * (n + 1));
}

static void ret(CPU *cpu, unsigned popped, uint32_t value) {
    cpu->r[0] = value;
    cpu->r[4] += 4 * (popped + 1);
}

void f_A51210(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x67326d);
    const uint32_t frame = arg(cpu, 0);
    uint32_t address = title_equals(frame, stock_path) ? STOCK : REAL_VOICE;
    assert(title_equals(frame, stock_path) || title_equals(frame, wav_path));
    in_guest(address, address == STOCK ? stock_path : wav_path);
    M32(cpu->r[1]) = address;
    ret(cpu, 1, cpu->r[1]);
}

void f_401EA0(CPU *cpu) { assert(arg(cpu, 0) == 0x93); ret(cpu, 0, 0); }
void f_4D6770(CPU *cpu) { assert(arg(cpu, 0) == STOCK || arg(cpu, 0) == REAL_VOICE); ret(cpu, 1, AUDIO_DEFINITION); }
void f_A42320(CPU *cpu) { ret(cpu, 1, 0); }
void f_B9A5D0(CPU *cpu) { ret(cpu, 0, AUDIO_MANAGER); }
void f_B9CA90(CPU *cpu) {
    assert(cpu->r[1] == AUDIO_MANAGER && arg(cpu, 0) == AUDIO_TASK && arg(cpu, 2) == 0x90 && arg(cpu, 3) == 0x80);
    assert(arg(cpu, 4) == STOCK || arg(cpu, 4) == REAL_VOICE);
    ++host.voice_submissions;
    if (host.change_voice_after_submit) M32(MARKED + 0x10) = REAL_VOICE;
    if (host.replace_response_during_submit) {
        SkyrimFuzLiftChoice accepted = {.force_subtitle = true};
        memcpy(accepted.assigned_path, stock_path, sizeof stock_path);
        assert(skyrim_fuz_menu_assigned(&host.responses, &host.memory, MARKED, &accepted));
    }
    ret(cpu, 5, 1);
}
void f_B9A2E0(CPU *cpu) { (void)cpu; assert(!"Actor effect route outside fixture"); }
void f_B9A0F0(CPU *cpu) { (void)cpu; assert(!"Actor effect route outside fixture"); }
void f_B99FE0(CPU *cpu) { assert(cpu->r[1] == AUDIO_TASK); ret(cpu, 1, 0); }
void f_B99ED0(CPU *cpu) { assert(cpu->r[1] == AUDIO_TASK); ret(cpu, 0, 0); }
void f_40E2F0(CPU *cpu) { assert(cpu->r[1] == ACTOR + 0x48); ret(cpu, 0, 0); }
void f_4D6180(CPU *cpu) { (void)cpu; assert(!"No voice route outside fixture"); }

void f_A4D870(CPU *cpu) {
    assert(arg(cpu, 0) == TITLE && M32(TLS_TAG) == 0x43);
    ret(cpu, 0, host.reject_title);
}

void f_65CD20(CPU *cpu) {
    assert(arg(cpu, 1) == ACTOR);
    M32(arg(cpu, 0)) = ACTOR_HANDLE;
    ret(cpu, 0, arg(cpu, 0));
}

void f_A48D60(CPU *cpu) {
    assert(cpu->r[1] == HEAP && !arg(cpu, 1) && !arg(cpu, 2));
    const uint32_t caller = M32(cpu->r[4]);
    if (caller == 0xa49c88) {
        assert(arg(cpu, 0) == INITIAL_CAPACITY * ROW_BYTES && !host.initial_rows_live);
        host.initial_rows_live = true;
        ret(cpu, 3, ROWS);
        return;
    }
    if (caller == 0xa49d0c) {
        assert(arg(cpu, 0) == 2 * INITIAL_CAPACITY * ROW_BYTES && host.initial_rows_live && !host.grown_rows_live);
        host.grown_rows_live = !host.fail_growth;
        ret(cpu, 3, host.fail_growth ? 0 : GROWN_ROWS);
        return;
    }
    assert(caller == 0x402c57 && arg(cpu, 0) == sizeof title);
    if (!host.temporary) {
        host.temporary = true;
        ret(cpu, 3, TEMP_TITLE);
        return;
    }
    if (host.fail_copy) { ret(cpu, 3, 0); return; }
    for (unsigned i = 0; i < MAX_OWNED; ++i) {
        if (host.owned[i]) continue;
        host.owned[i] = true;
        ret(cpu, 3, OWNED_TITLES + i * TITLE_STRIDE);
        return;
    }
    assert(!"Authored heap exhausted");
}

static void copy_memory(CPU *cpu) {
    const uint32_t dest = arg(cpu, 0), capacity = arg(cpu, 1), source = arg(cpu, 2), count = arg(cpu, 3);
    assert(count <= capacity);
    void *to = gm_pointer(&host.space, dest, count);
    const void *from = gm_pointer(&host.space, source, count);
    assert(to && from);
    memmove(to, from, count);
    ret(cpu, 0, 0);
}
void f_F522BA(CPU *cpu) { copy_memory(cpu); }
void f_F52317(CPU *cpu) { copy_memory(cpu); }

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    if (target == 0x4016b0 && M32(cpu->r[4]) == 0xa49baf) {
        f_4016B0(cpu);
        return;
    }
    if (target == 0x4016c0 && M32(cpu->r[4]) == 0xa49bd2) {
        f_4016C0(cpu);
        return;
    }
    assert(!"Unexpected external actor/helper path");
}

void f_897480(CPU *cpu) { (void)cpu; assert(!"No active HUD in this fixture"); }
void f_431B00(CPU *cpu) { (void)cpu; assert(!"No active HUD in this fixture"); }
void f_6F48A0(CPU *cpu) { assert(cpu->r[1] == NODE); ret(cpu, 0, !M32(NODE)); }
void f_A511B0(CPU *cpu) {
    assert(cpu->r[1] == MARKED + 0x10 && !host.responses.count && M32(cpu->r[1]) == STOCK);
    M32(cpu->r[1]) = 0;
    ret(cpu, 0, 0);
}
void f_48CB30(CPU *cpu) {
    assert(cpu->r[1] == NODE && !M32(NODE) && !M32(NODE + 4) && !arg(cpu, 0) && !arg(cpu, 1));
    ret(cpu, 2, 0);
}

void f_A487B0(CPU *cpu) {
    assert(cpu->r[1] == HEAP && !arg(cpu, 1));
    uint32_t pointer = arg(cpu, 0);
    if (pointer == TEMP_TITLE) {
        assert(host.temporary);
        host.temporary = false;
        memset(MEM + pointer, 0, sizeof title);
        ++host.temp_releases;
    } else if (pointer == ROWS || pointer == GROWN_ROWS) {
        bool *live = pointer == ROWS ? &host.initial_rows_live : &host.grown_rows_live;
        const size_t count = pointer == ROWS ? INITIAL_CAPACITY : 2 * INITIAL_CAPACITY;
        assert(*live);
        *live = false;
        memset(MEM + pointer, 0, count * ROW_BYTES);
        ++host.array_releases;
    } else if (pointer == TITLE || pointer == MARKED) {
        memset(MEM + pointer, 0, pointer == TITLE ? sizeof title : 0x24);
        ++host.response_releases;
    } else {
        assert(pointer >= OWNED_TITLES && (pointer - OWNED_TITLES) % TITLE_STRIDE == 0);
        unsigned slot = (pointer - OWNED_TITLES) / TITLE_STRIDE;
        assert(slot < MAX_OWNED && host.owned[slot]);
        for (size_t i = 0; i < host.chatter.count; ++i) assert(host.chatter.active[i].owned_title != pointer);
        host.owned[slot] = false;
        memset(MEM + pointer, 0, sizeof title);
        ++host.title_releases;
    }
    ret(cpu, 2, 0);
}

static CPU start_cpu(void) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    cpu.fs_base = FS_BASE;
    M32(TLS_TAG) = 0x12345678;
    return cpu;
}

static void response(uint32_t object, bool has_voice) {
    CPU cpu = start_cpu();
    cpu.r[1] = object;
    f_673170(&cpu);
    assert(cpu.r[4] == STACK + 4);
    in_guest(TITLE, title);
    M32(object) = TITLE;
    M16(object + 4) = sizeof title - 1;
    M16(object + 6) = sizeof title;
    host.original_voice = has_voice;
    cpu = start_cpu();
    cpu.r[1] = object + 0x10;
    cpu.r[6] = object;
    uint32_t frame = STACK + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT;
    M32(STACK) = frame;
    memset(MEM + frame, 0, SKYRIM_FUZ_LIFT_FRAME_BYTES);
    in_guest(frame, wav_path);
    skyrim_fuz_probe_voice_assignment(&cpu);
    assert(cpu.r[4] == STACK + 4 && M32(object + 0x10) == (has_voice ? REAL_VOICE : STOCK));
}

static void source(uint32_t object, bool legacy_force) {
    CPU cpu = start_cpu();
    cpu.r[5] = object;
    cpu.r[7] = ACTOR;
    M32(STACK + 0x130) = AUDIO_TASK;
    M32(STACK + 0x138) = legacy_force ? M32(PLAYER) : 0;
    M32(STACK + 0x13c) = 0;
    skyrim_fuz_probe_world_source(&cpu);
    assert(cpu.r[4] == STACK && M32(TLS_TAG) == 0x12345678 && !host.temporary);
}

static unsigned display(unsigned index, bool observed) {
    CPU cpu = start_cpu();
    cpu.r[3] = 1; // Original range comparison's mask.
    cpu.r[5] = M32(ARRAY) + index * ROW_BYTES;
    cpu.r[6] = QUEUE;
    if (observed) skyrim_fuz_probe_world_display(&cpu);
    else skyrim_fuz_probe_original_world_display(&cpu);
    assert(cpu.r[4] == STACK);
    return cpu.r[0];
}

static void swap_records(void) {
    CPU cpu = start_cpu();
    M32(STACK + 4) = ROWS;
    M32(STACK + 8) = ROWS + ROW_BYTES;
    M32(STACK + 12) = SWAP;
    M32(STACK + 16) = ROW_BYTES;
    f_A49DC0(&cpu);
    assert(cpu.r[4] == STACK + 4);
}

static void erase_record(unsigned index, void (*release)(CPU *)) {
    uint32_t record = ROWS + index * ROW_BYTES;
    const unsigned count = M32(ARRAY + 8);
    assert(index < count);
    if (M32(record + 4)) {
        CPU cpu = start_cpu();
        if (release) {
            cpu.r[1] = HEAP;
            M32(STACK) = M32(record + 4);
            M32(STACK + 4) = 0;
            release(&cpu);
            assert(cpu.r[4] == STACK + 8);
            M32(record + 4) = 0; // Same surrounding engine cleanup.
        } else {
            cpu.r[1] = ARRAY;
            M32(STACK + 4) = index;
            M32(STACK + 8) = 1;
            f_6963E0(&cpu);
            assert(cpu.r[4] == STACK + 12 && !M32(record + 4));
        }
    }
    if (count > index + 1) {
        CPU cpu = start_cpu();
        M32(STACK + 4) = ROWS;
        M32(STACK + 8) = index;
        M32(STACK + 12) = index + 1;
        M32(STACK + 16) = count - index - 1;
        M32(STACK + 20) = ROW_BYTES;
        f_A4A070(&cpu);
        assert(cpu.r[4] == STACK + 24);
    }
    M32(ARRAY + 8) = count - 1;
}

static void release_response(void) {
    CPU cpu = start_cpu();
    cpu.r[1] = NODE;
    M32(NODE) = MARKED;
    M32(NODE + 4) = 0;
    f_672EA0(&cpu);
    assert(cpu.r[4] == STACK + 4 && !host.responses.count && !M32(NODE));
}

static void scenarios(void) {
    response(MARKED, false);
    response(ORDINARY, true);
    assert(host.responses.count == 1);
    M32(ORDINARY + 0x10) = STOCK; // Explicitly indistinguishable bytes and reference, but a different response.
    source(ORDINARY, false);
    assert(!M32(ARRAY + 8) && !host.chatter.count && host.voice_submissions == 1);

    host.change_voice_after_submit = true;
    source(MARKED, false);
    assert(!M32(ARRAY + 8) && !host.chatter.count);
    host.change_voice_after_submit = false;
    M32(MARKED + 0x10) = STOCK;

    host.replace_response_during_submit = true;
    source(MARKED, false);
    assert(!M32(ARRAY + 8) && !host.chatter.count);
    host.replace_response_during_submit = false;

    host.reject_title = true;
    source(MARKED, false);
    assert(!M32(ARRAY + 8) && !host.chatter.count);
    host.reject_title = false;

    SkyrimFuzWorldScope scope = skyrim_fuz_world_capture(&host.responses, &host.memory, MARKED, STOCK);
    CPU cpu = start_cpu();
    M32(STACK) = 0x004e1b5f; // Another original chatter caller.
    M32(STACK + 4) = ACTOR;
    M32(STACK + 8) = TITLE;
    M32(STACK + 12) = 0;
    skyrim_fuz_host_queue_world(&cpu, &scope);
    assert(cpu.r[4] == STACK + 16 && !M32(ARRAY + 8));

    source(MARKED, false);
    assert(M32(ARRAY + 8) == 1 && host.chatter.count == 1 && host.records_copied == 1 && host.temp_releases == 1);
    assert(!M8(ROWS + 0x10) && MF32(ROWS + 0xc) == FLT_MAX && M32(ROWS) == ACTOR_HANDLE);
    assert(M32(ROWS + 4) != TITLE && M32(ROWS + 4) != TEMP_TITLE && title_equals(M32(ROWS + 4), title));
    MF32(ROWS + 0xc) = 25.0f;
    assert(display(0, true) == OUT_OF_RANGE);
    MF32(ROWS + 0xc) = 4.0f;
    assert(display(0, false) == SETTINGS_OFF && display(0, true) == SHOW_GENERAL);
    M32(QUEUE + 0x10) = ACTOR_HANDLE;
    assert(display(0, true) == REPEATED_SPEAKER);
    M32(QUEUE + 0x10) = none_handle;

    M8(GENERAL_SETTING) = 1;
    source(ORDINARY, true);
    assert(M32(ARRAY + 8) == 2 && host.chatter.count == 1 && M8(ROWS + ROW_BYTES + 0x10) == 1);
    assert(display(1, false) == SHOW_GENERAL); // The original force flag still bypasses its original range.
    M8(GENERAL_SETTING) = 0;
    assert(display(1, true) == SETTINGS_OFF);
    const uint32_t first_title = M32(ROWS + 4);
    swap_records();
    assert(M32(ROWS + ROW_BYTES + 4) == first_title && display(1, true) == SHOW_GENERAL);
    assert(display(0, true) == SETTINGS_OFF);

    release_response();
    assert(host.response_releases == 2 && host.chatter.count == 1 && !M8(TITLE));
    assert(title_equals(first_title, title) && display(1, true) == SHOW_GENERAL);
    erase_record(1, NULL);
    assert(!host.chatter.count && host.title_releases == 1);

    response(MARKED, true);
    M32(MARKED + 0x10) = STOCK; // A new response at the same object and heap-title addresses.
    M8(GENERAL_SETTING) = 1;
    source(MARKED, false);
    assert(M32(ROWS + ROW_BYTES + 4) == first_title);
    MF32(ROWS + ROW_BYTES + 0xc) = 4.0f;
    M8(GENERAL_SETTING) = 0;
    assert(display(1, true) == SETTINGS_OFF);
    erase_record(0, skyrim_fuz_probe_world_release_892E50);
    erase_record(0, skyrim_fuz_probe_world_release_893070);
    assert(!M32(ARRAY + 8) && !host.chatter.count && host.title_releases == 3);

    response(MARKED, false);
    host.fail_copy = true;
    source(MARKED, false);
    assert(M32(ARRAY + 8) == 1 && !host.chatter.count && !M32(ROWS + 4));
    erase_record(0, NULL);
    host.fail_copy = false;

    for (unsigned i = 0; i < 2; ++i) {
        source(MARKED, false);
        assert(host.chatter.count == 1);
        erase_record(0, i ? skyrim_fuz_probe_world_release_893070 : skyrim_fuz_probe_world_release_892E50);
        assert(!host.chatter.count);
    }
    assert(!host.temporary && !M32(ARRAY + 8) && host.title_releases == 5);
    for (unsigned i = 0; i < MAX_OWNED; ++i) assert(!host.owned[i]);
}

static void queue_growth_and_reset(void) {
    assert(M32(ARRAY) == ROWS && M32(ARRAY + 4) == INITIAL_CAPACITY);
    for (unsigned i = 0; i < INITIAL_CAPACITY; ++i) source(MARKED, false);
    assert(M32(ARRAY + 8) == INITIAL_CAPACITY && host.chatter.count == INITIAL_CAPACITY);
    const uint32_t title_pointer = M32(ROWS + 4);
    MF32(ROWS + 0xc) = 4.0f;

    host.fail_growth = true;
    source(MARKED, false);
    assert(M32(ARRAY) == ROWS && M32(ARRAY + 8) == INITIAL_CAPACITY && !host.array_releases);
    assert(host.chatter.count == INITIAL_CAPACITY && display(0, true) == SHOW_GENERAL);

    host.fail_growth = false;
    source(MARKED, false);
    assert(M32(ARRAY) == GROWN_ROWS && M32(ARRAY + 8) == INITIAL_CAPACITY + 1 && host.array_releases == 1);
    assert(!host.initial_rows_live && host.grown_rows_live && host.chatter.count == INITIAL_CAPACITY + 1);
    assert(M32(GROWN_ROWS + 4) == title_pointer && display(0, true) == SHOW_GENERAL);
    assert(!skyrim_fuz_world_general(&host.chatter, &host.memory, ARRAY, ROWS));

    CPU cpu = start_cpu();
    cpu.r[1] = QUEUE;
    f_892DA0(&cpu);
    assert(cpu.r[4] == STACK + 4 && M32(TLS_TAG) == 0x12345678);
    assert(!M32(ARRAY) && !M32(ARRAY + 4) && !M32(ARRAY + 8) && !host.grown_rows_live);
    assert(!host.chatter.count && host.title_releases == 5 + INITIAL_CAPACITY + 1 && host.array_releases == 2);
    for (unsigned i = 0; i < MAX_OWNED; ++i) assert(!host.owned[i]);
}

int main(void) {
    assert(gm_reserve(&host.space, NULL));
    MEM = host.space.base;
    const uint32_t pages[] = {FS_BASE, STACK, TLS_KEY, ARRAY_OPERATIONS, GENERAL_SETTING, QUEUE_SLOT,
                              INVALID_HANDLE, PLAYER};
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; ++i) commit(pages[i]);
    M32(FS_BASE + 0x2c) = TLS_VECTOR;
    M32(TLS_VECTOR) = TLS_DATA;
    M32(TLS_KEY) = 0;
    M32(QUEUE_SLOT) = QUEUE;
    M32(PLAYER) = 0x5000;
    M32(ARRAY_OPERATIONS) = 0x4016b0;
    M32(ARRAY_OPERATIONS + 4) = 0x4016c0;
    M32(INVALID_HANDLE) = none_handle;
    M32(QUEUE + 0x10) = none_handle;
    MF32(DISTANCE_SETTING) = 3.0f;
    M8(GENERAL_SETTING) = M8(DIALOGUE_SETTING) = 0;
    host.memory = (SkyrimFuzLiftMemory){read_guest, replace_guest, &host.space};
    host.policy = (SkyrimFuzPolicy){.lookup = find_resource, .context = &host, .words_per_second = 2,
                                  .skip_empty_responses = true};
    host.policy.stock_paths[3] = stock_resource;

    scenarios();
    queue_growth_and_reset();
    assert(gm_release(&host.space, NULL));
    puts("World chatter retained exact copied title ownership and used the original range policy.");
    return 0;
}
