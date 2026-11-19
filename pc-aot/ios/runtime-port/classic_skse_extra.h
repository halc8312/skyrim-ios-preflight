/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* TESV 1.9.32 gameplay context and device layouts from the Classic 1.7.3 SDK.
 * The VM and the control map both store names as interned BSFixedString handles. */
enum {
    CLASSIC_INPUT_MANAGER = 0x012e7458,
    CLASSIC_INPUT_DISPATCHER = 0x01b2e724,
    CLASSIC_GAMEPLAY_CONTEXT = 0x34,
    CLASSIC_GAMEPAD_OBJECT = 0x3c,
    CLASSIC_IS_ENABLED_SLOT = 3,
    CLASSIC_KEYBOARD = 0, CLASSIC_MOUSE = 1, CLASSIC_GAMEPAD = 2,
    CLASSIC_UNBOUND = 255, CLASSIC_AUTO_DEVICE = 255,
    CLASSIC_MOUSE_BASE = 256, CLASSIC_GAMEPAD_BASE = 266, CLASSIC_KEY_LIMIT = 282,
    CLASSIC_MAPPING_NAME = 0, CLASSIC_MAPPING_BUTTON = 4, CLASSIC_MAPPING_SIZE = 16
};

/* The last two entries represent the triggers. */
static const uint16_t skse_classic_gamepad_buttons[] = {
    1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80,
    0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000, 9, 10
};

static uint32_t skse_classic_input_field(uint32_t address, unsigned offset) {
    if (!address || address > UINT32_MAX - offset - 3u) return 0;
    return M32(address + offset);
}

static int skse_classic_gamepad_enabled(CPU *cpu) {
    uint32_t dispatcher = M32(CLASSIC_INPUT_DISPATCHER);
    uint32_t pad = skse_classic_input_field(dispatcher, CLASSIC_GAMEPAD_OBJECT);
    uint32_t vtable = skse_classic_input_field(pad, 0);
    if (!skse_classic_input_field(vtable, CLASSIC_IS_ENABLED_SLOT * 4)) return 0;
    /* The x86 method returns a C++ bool in AL. */
    return (uint8_t)skse_virtual(cpu, pad, CLASSIC_IS_ENABLED_SLOT, 0, NULL) != 0;
}

static uint32_t skse_classic_mapping(uint32_t context, unsigned device,
        unsigned field, uint32_t value) {
    /* Each device owns a tArray of {interned name, button ID, ...}. */
    unsigned offset = device * 12;
    uint32_t entries = skse_classic_input_field(context, offset);
    uint32_t capacity = skse_classic_input_field(context, offset + 4);
    uint32_t count = skse_classic_input_field(context, offset + 8);
    if (!entries || count > capacity ||
        (uint64_t)entries + (uint64_t)count * CLASSIC_MAPPING_SIZE > UINT64_C(0x100000000)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = entries + i * CLASSIC_MAPPING_SIZE;
        if (M32(entry + field) == value) return entry;
    }
    return 0;
}

static uint32_t skse_classic_mapped_key(CPU *cpu, uint32_t context,
        uint32_t name, uint32_t device) {
    if (!context) return UINT32_MAX;
    int automatic = device == CLASSIC_AUTO_DEVICE;
    if (automatic) device = skse_classic_gamepad_enabled(cpu) ? CLASSIC_GAMEPAD : CLASSIC_KEYBOARD;
    /* The original defaults unknown manual device types to the keyboard map. */
    if (device > CLASSIC_GAMEPAD) device = CLASSIC_KEYBOARD;
    uint32_t entry = skse_classic_mapping(context, device, CLASSIC_MAPPING_NAME, name);
    uint32_t button = entry ? M32(entry + CLASSIC_MAPPING_BUTTON) : CLASSIC_UNBOUND;
    if (automatic && device == CLASSIC_KEYBOARD && button == CLASSIC_UNBOUND) {
        device = CLASSIC_MOUSE;
        entry = skse_classic_mapping(context, device, CLASSIC_MAPPING_NAME, name);
        button = entry ? M32(entry + CLASSIC_MAPPING_BUTTON) : CLASSIC_UNBOUND;
    }
    if (button == CLASSIC_UNBOUND) return UINT32_MAX;
    if (device == CLASSIC_MOUSE) return CLASSIC_MOUSE_BASE + button;
    if (device == CLASSIC_GAMEPAD) {
        for (unsigned i = 0; i < CLASSIC_KEY_LIMIT - CLASSIC_GAMEPAD_BASE; ++i)
            if (skse_classic_gamepad_buttons[i] == button) return CLASSIC_GAMEPAD_BASE + i;
        return UINT32_MAX;
    }
    return button;
}

static uint32_t skse_classic_mapped_name(uint32_t context, uint32_t key) {
    if (!context || key == CLASSIC_UNBOUND || key >= CLASSIC_KEY_LIMIT) return 0;
    unsigned device = CLASSIC_KEYBOARD;
    if (key >= CLASSIC_GAMEPAD_BASE) {
        device = CLASSIC_GAMEPAD;
        key = skse_classic_gamepad_buttons[key - CLASSIC_GAMEPAD_BASE];
    } else if (key >= CLASSIC_MOUSE_BASE) {
        device = CLASSIC_MOUSE;
        key -= CLASSIC_MOUSE_BASE;
    }
    uint32_t entry = skse_classic_mapping(context, device, CLASSIC_MAPPING_BUTTON, key);
    return entry ? M32(entry + CLASSIC_MAPPING_NAME) : 0;
}

/* skse_system_run has initialized a typed, empty VM result. Neither lookup
 * changes device mode, bindings or guest input. */
static void skse_classic_input_query(CPU *cpu, unsigned op, const uint32_t *args, uint32_t result) {
    uint32_t context = skse_classic_input_field(M32(CLASSIC_INPUT_MANAGER), CLASSIC_GAMEPLAY_CONTEXT);
    if (op == S_INPUT_KEY) {
        M32(result + 4) = skse_classic_mapped_key(cpu, context, args[0], args[1]);
    } else {
        uint32_t name = skse_classic_mapped_name(context, args[0]);
        if (name) vm_string(cpu, result, str(name));
    }
}
