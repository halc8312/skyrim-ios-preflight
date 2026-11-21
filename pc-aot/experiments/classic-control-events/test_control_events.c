/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x2fe0, EVENT_HEAD = 0x2030, EVENTS = 0x2100, ACTION_NAMES = 0x2400,
    EVENT_BYTES = 0x20, ACTION_NAME_BYTES = 20, MAX_OBSERVATIONS = 16,
    KEYBOARD = 0, GAMEPAD = 2, PHYSICAL_KEY_E = 0x12,
    XINPUT_A = 0x1000, XINPUT_B = 0x2000, XINPUT_X = 0x4000,
    MACRO_A = 276, MACRO_B = 277, MACRO_X = 278,
};

typedef struct {
    uint32_t device, type, key, flags;
    float held_seconds;
    const char *action;
} Button;

typedef struct {
    unsigned key;
    int down;
    char action[ACTION_NAME_BYTES];
} GfxObservation;

typedef struct {
    int down;
    float held_seconds;
    uint32_t action_id;
    char action[ACTION_NAME_BYTES];
} ControlObservation;

uint8_t *MEM;
static GfxObservation original_gfx[MAX_OBSERVATIONS], adapted_gfx[MAX_OBSERVATIONS];
static ControlObservation captured[MAX_OBSERVATIONS];
static GfxObservation *gfx_records;
static unsigned gfx_count, control_count;
static bool mutate_name_after_gfx;

void skyrim_probe_original_input(CPU *cpu);
void skyrim_probe_control_input(CPU *cpu);
void f_667000(CPU *cpu);

static void copy_action(char destination[ACTION_NAME_BYTES], const char *action) {
    size_t length = strlen(action);
    assert(length < ACTION_NAME_BYTES);
    memcpy(destination, action, length + 1);
}

void skse_gfx_input_event(CPU *cpu, unsigned key, const char *action, int down) {
    assert(cpu->r[4] == STACK && gfx_count < MAX_OBSERVATIONS);
    GfxObservation *record = &gfx_records[gfx_count++];
    record->key = key;
    record->down = down;
    copy_action(record->action, action);
    // Model a nested UI remap changing borrowed data after the observation.
    if (mutate_name_after_gfx && *action) ((char *)action)[0] = '\0';
}

void skyrim_probe_control_edge(CPU *cpu, uint32_t action, int down, float held_seconds) {
    assert(cpu->r[4] == STACK && control_count < MAX_OBSERVATIONS);
    ControlObservation *record = &captured[control_count++];
    record->down = down;
    record->held_seconds = held_seconds;
    record->action_id = action;
    copy_action(record->action, action ? (const char *)MEM + action : "");
}

static void place_buttons(const Button *buttons, size_t count) {
    assert(count <= MAX_OBSERVATIONS);
    M32(EVENT_HEAD) = count ? EVENTS : 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t base = EVENTS + (uint32_t)i * EVENT_BYTES;
        uint32_t str = ACTION_NAMES + (uint32_t)i * ACTION_NAME_BYTES;
        const Button *button = &buttons[i];
        M32(base + 4) = button->device;
        M32(base + 8) = button->type;
        M32(base + 0xc) = i + 1 == count ? 0 : base + EVENT_BYTES;
        M32(base + 0x10) = button->action ? str : 0;
        M32(base + 0x14) = button->key;
        M32(base + 0x18) = button->flags;
        MF32(base + 0x1c) = button->held_seconds;
        if (button->action) copy_action((char *)MEM + str, button->action);
    }
}

static void observe(void (*observer)(CPU *)) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    M32(STACK + 4) = EVENT_HEAD;
    M32(STACK + 8) = 0;
    observer(&cpu);
    assert(cpu.r[0] == 0 && cpu.r[4] == STACK + 12);
}

int main(void) {
    gm_space memory = {0};
    assert(gm_reserve(&memory, NULL));
    assert(gm_commit_page(&memory, (EVENTS / memory.page_size) * memory.page_size, NULL));
    MEM = memory.base;
    const Button buttons[] = {
        {KEYBOARD, 0, PHYSICAL_KEY_E, 1, 0, "Activate"},
        {GAMEPAD, 0, XINPUT_A, 1, 0, "Activate"},
        {GAMEPAD, 0, XINPUT_A, 1, .625f, "Activate"}, // Held sample.
        {GAMEPAD, 0, XINPUT_A, 0, 1.375f, "Activate"},
        {GAMEPAD, 0, XINPUT_A, 1, 0, "Sprint"}, // Same raw button under a remap.
        {GAMEPAD, 0, XINPUT_B, 1, 0, "Activate"},
        {GAMEPAD, 0, XINPUT_X, 0, 0, "Ready Weapon"}, // No edge.
        {GAMEPAD, 0, XINPUT_X, 1, 0, "Ready Weapon"},
        {GAMEPAD, 0, XINPUT_X, 0, .8125f, "Ready Weapon"},
        {GAMEPAD, 1, XINPUT_A, 1, 0, "Activate"}, // Non-button event.
        {3, 0, XINPUT_A, 1, 0, "Activate"},
        {GAMEPAD, 0, 0x400, 1, 0, "Activate"}, // Unmapped macro mask.
        {KEYBOARD, 0, 0x39, 1, 0, NULL},
    };
    const size_t button_count = sizeof buttons / sizeof buttons[0];
    place_buttons(buttons, button_count);
    CPU accessor = {0};
    accessor.r[1] = EVENTS;
    accessor.r[4] = STACK;
    f_667000(&accessor);
    assert(accessor.r[0] == EVENTS + 0x10 && accessor.r[4] == STACK + 4);
    assert(M32(accessor.r[0]) == ACTION_NAMES);

    gfx_records = original_gfx;
    observe(skyrim_probe_original_input);
    const unsigned original_count = gfx_count;
    assert(original_count == 8 && !control_count);

    gfx_records = adapted_gfx;
    gfx_count = 0;
    mutate_name_after_gfx = true;
    observe(skyrim_probe_control_input);
    assert(gfx_count == original_count && control_count == original_count);
    const unsigned expected_keys[] = {PHYSICAL_KEY_E, MACRO_A, MACRO_A, MACRO_A, MACRO_B, MACRO_X, MACRO_X, 0x39};
    const unsigned accepted[] = {0, 1, 3, 4, 5, 7, 8, 12};
    for (unsigned i = 0; i < original_count; ++i) {
        const Button *button = &buttons[accepted[i]];
        const char *action = button->action ? button->action : "";
        assert(original_gfx[i].key == expected_keys[i] && adapted_gfx[i].key == original_gfx[i].key);
        assert(!strcmp(original_gfx[i].action, action) && !strcmp(adapted_gfx[i].action, action));
        assert(original_gfx[i].down == (button->flags != 0) && adapted_gfx[i].down == original_gfx[i].down);
        assert(!strcmp(captured[i].action, action) && captured[i].down == original_gfx[i].down);
        assert(captured[i].held_seconds == button->held_seconds);
        assert(captured[i].action_id == (button->action ? ACTION_NAMES + accepted[i] * ACTION_NAME_BYTES : 0));
    }
    place_buttons(NULL, 0);
    observe(skyrim_probe_control_input);
    assert(gfx_count == original_count && control_count == original_count);

    assert(gm_release(&memory, NULL));
    puts("Reviewed LE button observer preserved GFX input and captured the engine action and timer.");
    return 0;
}
