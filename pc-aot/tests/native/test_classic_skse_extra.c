/* SPDX-License-Identifier: GPL-3.0-only */
#include "skse_compat.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

enum {
    RESULT = 16, MANAGER = 0x80, CONTEXT = 0x100, DISPATCHER = 0x200,
    PAD = 0x300, VTABLE = 0x400, KEYBOARD_MAP = 0x500, MOUSE_MAP = 0x700,
    PAD_MAP = 0x900, ACTION = 0xc00, OTHER = 0xc20, WHEEL = 0xc40
};
typedef struct { uint32_t enabled, calls; } CPU;
static CPU cpu;
static uint32_t memory[0x1800 / 4];
static uint32_t manager_slot, dispatcher_slot;

static uint32_t *word(uint32_t address) {
    if (address == 0x012e7458) return &manager_slot;
    if (address == 0x01b2e724) return &dispatcher_slot;
    assert(address && address % 4 == 0 && address <= sizeof memory - 4);
    return &memory[address / 4];
}
#define M32(address) (*word(address))
static const char *str(uint32_t name) {
    assert(name < sizeof memory);
    return (const char *)memory + name;
}
static void vm_string(CPU *state, uint32_t result, const char *text) {
    assert(state == &cpu && result == RESULT && M32(result) == 2 && !M32(result + 4));
    M32(result + 4) = (uint32_t)(text - (const char *)memory);
}
static uint32_t skse_virtual(CPU *state, uint32_t object, unsigned slot, int argc, const uint32_t *args) {
    assert(state == &cpu && object == PAD && slot == 3 && argc == 0 && !args);
    ++state->calls;
    return state->enabled;
}
#include "classic_skse_extra.h"

static const SkseNative *native(const char *name) {
    unsigned count;
    const SkseNative *table = skse_native_table(&count), *found = NULL;
    for (unsigned i = 0; i < count; ++i) {
        if (!strcmp(table[i].class_name, "Input") && !strcmp(table[i].name, name)) {
            assert(!found);
            found = &table[i];
        }
    }
    assert(found);
    return found;
}
static uint32_t call(const SkseNative *fn, uint32_t first, uint32_t second) {
    uint32_t args[4] = {first, second, 0, 0};
    M32(RESULT) = fn->result;
    M32(RESULT + 4) = 0;
    skse_classic_input_query(&cpu, fn->operation, args, RESULT);
    assert(M32(RESULT) == fn->result);
    return M32(RESULT + 4);
}
static void install_map(unsigned device, uint32_t addr, unsigned size) {
    uint32_t array = CONTEXT + device * 12;
    M32(array) = addr;
    M32(array + 4) = size + 2;
    M32(array + 8) = size;
}
static void binding(uint32_t map, unsigned pos, uint32_t name, uint32_t key) {
    M32(map + 16 * pos) = name;
    M32(map + 16 * pos + 4) = key;
}

int main(void) {
    const SkseNative *key = native("GetMappedKey"), *control = native("GetMappedControl");
    assert(key->result == 3 && key->argc == 2 && key->args[0] == 2 && key->args[1] == 3);
    assert(control->result == 2 && control->argc == 1 && control->args[0] == 3);
    assert(key->operation == S_INPUT_KEY && control->operation == S_INPUT_CONTROL);
    assert(call(key, ACTION, 255) == UINT32_MAX && call(control, 0, 0) == 0);

    manager_slot = MANAGER;
    M32(MANAGER + 0x34) = CONTEXT;
    dispatcher_slot = DISPATCHER;
    M32(DISPATCHER + 0x3c) = PAD;
    M32(PAD) = VTABLE;
    M32(VTABLE + 12) = 1;
    strcpy((char *)memory + ACTION, "Action");
    strcpy((char *)memory + OTHER, "Secondary");
    strcpy((char *)memory + WHEEL, "Mouse Wheel");
    install_map(0, KEYBOARD_MAP, 4);
    install_map(1, MOUSE_MAP, 10);
    install_map(2, PAD_MAP, 16);
    binding(KEYBOARD_MAP, 0, ACTION, 0);
    binding(KEYBOARD_MAP, 1, ACTION, 30);
    binding(KEYBOARD_MAP, 2, OTHER, 30);
    binding(KEYBOARD_MAP, 3, WHEEL, 255);
    for (unsigned i = 0; i < 10; ++i) {
        uint32_t name = i == 0 ? ACTION : i == 9 ? WHEEL : 0xe00 + 4 * i;
        binding(MOUSE_MAP, i, name, i);
        assert(call(key, name, 1) == 256 + i);
        assert(call(control, 256 + i, 0) == name);
    }
    const uint32_t buttons[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 4096, 8192, 16384, 32768, 9, 10};
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        uint32_t name = i == 10 ? ACTION : 0xf00 + 4 * i;
        binding(PAD_MAP, i, name, buttons[i]);
        assert(call(key, name, 2) == 266 + i);
        assert(call(control, 266 + i, 0) == name);
    }
    assert(!cpu.calls); /* Manual selection and key-to-control never query device mode. */
    assert(call(key, ACTION, 0) == 0 && call(key, ACTION, 99) == 0);
    assert(call(control, 30, UINT32_MAX) == ACTION); /* first duplicate button wins */
    assert(!call(control, 255, 0) && !call(control, 282, 0) && !call(control, UINT32_MAX, 0));
    assert(call(key, WHEEL, 0) == UINT32_MAX);
    assert(call(key, 0x1234, 0) == UINT32_MAX);
    cpu.enabled = 0x123400;
    assert(call(key, ACTION, 255) == 0 && call(key, WHEEL, 255) == 265);
    cpu.enabled = 0x123401;
    assert(call(key, ACTION, 255) == 276 && call(key, WHEEL, 255) == UINT32_MAX);
    assert(cpu.calls == 4);
    binding(PAD_MAP, 10, ACTION, 3);
    assert(call(key, ACTION, 2) == UINT32_MAX);
    binding(KEYBOARD_MAP, 0, ACTION, 42);
    assert(call(key, ACTION, 0) == 42 && call(control, 42, 0) == ACTION);

    dispatcher_slot = 0;
    assert(call(key, WHEEL, 255) == 265 && cpu.calls == 4);
    dispatcher_slot = DISPATCHER;
    M32(DISPATCHER + 0x3c) = 0;
    assert(call(key, WHEEL, 255) == 265 && cpu.calls == 4);
    M32(CONTEXT + 4) = 0; /* incomplete tArray */
    assert(call(key, ACTION, 0) == UINT32_MAX && !call(control, 42, 0));
    install_map(0, UINT32_MAX - 3, 4); /* wrapped guest range */
    assert(call(key, ACTION, 0) == UINT32_MAX && !call(control, 42, 0));
    M32(MANAGER + 0x34) = 0;
    assert(call(key, ACTION, 255) == UINT32_MAX && cpu.calls == 4);
    return 0;
}
