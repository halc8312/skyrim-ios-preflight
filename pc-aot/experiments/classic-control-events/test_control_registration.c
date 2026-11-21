/* SPDX-License-Identifier: GPL-3.0-only */
#include "control_bridge.h"
#include "guest_memory.h"
#include "lift_prelude.h"
#include "skse_compat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    STACK = 0x3f00, EVENTS = 0x3100, EVENT_HEAD = 0x3400, EVENT_SIZE = 0x20,
    ACTIVATE = 0x3500, SPRINT = 0x3520, READY = 0x3540, SEPARATE_INTERN = 0x3560,
    VM = 0x3600, OTHER_VM = 0x3610, REGISTRY = 0x3650,
    NATIVE = 0x3700, BASE = 0x3740, OBJECT = 0x3760, RESULT = 0x3780,
    STATE = 0x37a0, LIST = 0x37e0, ARGUMENT = 0x3800,
    KEYBOARD = 0, GAMEPAD = 2, KEY_E = 0x12, BUTTON_A = 0x1000, BUTTON_X = 0x4000,
    POLICY_ADDRESS = 0x01ba7008, POLICY = 0x3900, DELETE_GLOBAL = 0x01b40460,
    STRING_TYPE = 2, INTEGER_TYPE = 3, FLOAT_TYPE = 4,
    CONTROL_OPERATION_BASE = 1990,
};
static const uint64_t handles[] = {
    UINT64_C(0x0000007aabcdef01), UINT64_C(0x1000007aabcdef01), UINT64_C(0x2000007aabcdef01),
};
static const uint32_t actions[] = {ACTIVATE, SPRINT, READY, SEPARATE_INTERN};
static const char *const action_names[] = {"Activate", "Sprint", "Ready Weapon", "Activate"};

#define NATIVE_ROW(receiver, name, argc, type, op) \
    {receiver, name, 0, argc, {type, 0, 0, 0}, S_METHOD | (CONTROL_OPERATION_BASE + op)}
#define CONTROL_METHODS(receiver) \
    NATIVE_ROW(receiver, "RegisterForControl", 1, STRING_TYPE, CLASSIC_REGISTER_CONTROL), \
    NATIVE_ROW(receiver, "UnregisterForControl", 1, STRING_TYPE, CLASSIC_UNREGISTER_CONTROL), \
    NATIVE_ROW(receiver, "UnregisterForAllControls", 0, 0, CLASSIC_UNREGISTER_ALL_CONTROLS)
static const SkseNative natives[] = {
    CONTROL_METHODS("Form"), CONTROL_METHODS("Alias"), CONTROL_METHODS("ActiveMagicEffect"),
};
#undef CONTROL_METHODS
#undef NATIVE_ROW

typedef struct {
    uint64_t handle;
    const char *name;
    unsigned argc, types[2];
    char action[20];
    float held_seconds;
} QueuedEvent;

typedef struct {
    ClassicControlBridge bridge;
    CPU reference_cpu;
    QueuedEvent events[16];
    unsigned event_count, gfx_count;
    unsigned handle_refs[3], queued_refs[3], action_refs[4];
    unsigned delete_calls;
    bool vm_active, mutate_after_gfx;
} AuthoredVm;

typedef struct {
    uint32_t device, button, action, flags;
    float elapsed;
} Button;

uint8_t *MEM;
static AuthoredVm fixture;

void skyrim_probe_control_input(CPU *);
void skyrim_probe_control_native(CPU *);
uint64_t skyrim_probe_base_handle(uint32_t);
void skyrim_probe_handle_ref(CPU *, uint64_t, int);
void skyrim_probe_delete_original(CPU *);
void skyrim_probe_delete_adapted(CPU *);

static unsigned handle_index(uint64_t handle) {
    for (unsigned i = 0; i < sizeof handles / sizeof handles[0]; ++i)
        if (handles[i] == handle) return i;
    assert(!"Unrecognized VM handle");
    return 0;
}

static unsigned action_index(uint32_t action) {
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (actions[i] == action) return i;
    assert(!"Unrecognized guest interned string");
    return 0;
}

static void copy_text(char *target, size_t capacity, const char *source) {
    size_t length = strlen(source);
    assert(length < capacity);
    memcpy(target, source, length + 1);
}

const SkseNative *skse_native_table(unsigned *count) {
    *count = sizeof natives / sizeof natives[0];
    return natives;
}

uint32_t skse_member(CPU *cpu, uint32_t address, uint32_t self, int count, const uint32_t *args) {
    (void)cpu;
    if (address == 0xc3a620) {
        assert(self == LIST && count == 1 && args[0] == STATE);
        return 17;
    }
    assert(address == 0xc3ac40 && self == LIST && count == 3 &&
           args[0] == STATE && args[1] == 0 && args[2] == 17);
    return ARGUMENT;
}

uint32_t skse_virtual(CPU *cpu, uint32_t self, unsigned slot, int count, const uint32_t *args) {
    assert(cpu == &fixture.reference_cpu && fixture.vm_active);
    assert(self == POLICY && count == 2 && (slot == 9 || slot == 10));
    uint64_t handle = ((uint64_t)args[1] << 32) | args[0];
    unsigned *refs = &fixture.handle_refs[handle_index(handle)];
    if (slot == 9) ++*refs;
    else {
        assert(*refs > 0);
        --*refs;
    }
    return 0;
}

int skse_system_run(CPU *cpu, const SkseNative *native, uint32_t base, const uint32_t *args,
                    uint32_t registry, uint32_t result) {
    (void)cpu;
    assert(registry == REGISTRY && result == RESULT && (native->operation & S_METHOD));
    ClassicControlOperation op = (ClassicControlOperation)((native->operation & ~S_METHOD) - CONTROL_OPERATION_BASE);
    M32(result) = M32(result + 4) = 0;
    return classic_control_change(&fixture.bridge, op, skyrim_probe_base_handle(base), args[0]);
}

int skse_skyui_enabled(void) { assert(!"Unexpected primitive"); return 0; }
uint32_t gstrdup(const char *text) { (void)text; assert(!"Unexpected primitive"); return 0; }
void gfree(uint32_t address) { (void)address; assert(!"Unexpected primitive"); }
int skse_native_eval(const SkseNative *native, const uint32_t args[4], const char *strings[4],
                     uint32_t *value, char **string) {
    (void)native;
    (void)args;
    (void)strings;
    (void)value;
    (void)string;
    assert(!"Unexpected primitive");
    return 0;
}

static void reference_handle(void *context, uint64_t handle, bool retain) {
    AuthoredVm *vm = context;
    skyrim_probe_handle_ref(&vm->reference_cpu, handle, retain);
}

static void reference_action(void *context, uint32_t action, bool retain) {
    AuthoredVm *vm = context;
    unsigned *refs = &vm->action_refs[action_index(action)];
    if (retain) ++*refs;
    else {
        assert(*refs > 0);
        --*refs;
    }
}

static void enqueue(void *context, const ClassicControlEvent *event) {
    AuthoredVm *vm = context;
    assert(vm->vm_active && vm->event_count < sizeof vm->events / sizeof vm->events[0]);
    assert(vm->handle_refs[handle_index(event->handle)] > 0 && vm->action_refs[action_index(event->action)] > 0);
    assert((event->argc == 1 && !strcmp(event->name, "OnControlDown")) ||
           (event->argc == 2 && !strcmp(event->name, "OnControlUp")));
    QueuedEvent *record = &vm->events[vm->event_count++];
    *record = (QueuedEvent){
        .handle = event->handle, .name = event->name, .argc = event->argc,
        .types = {STRING_TYPE, event->argc == 2 ? FLOAT_TYPE : 0}, .held_seconds = event->held_seconds,
    };
    copy_text(record->action, sizeof record->action, (const char *)MEM + event->action);
    ++vm->queued_refs[handle_index(event->handle)];
}

void skyrim_probe_control_edge(CPU *cpu, uint32_t action, int down, float elapsed) {
    assert(cpu->r[4] == STACK);
    classic_control_edge(&fixture.bridge, action, down != 0, elapsed);
}

void skse_gfx_input_event(CPU *cpu, unsigned key, const char *action, int down) {
    assert(cpu->r[4] == STACK && key < 282 && (down == 0 || down == 1));
    ++fixture.gfx_count;
    if (fixture.mutate_after_gfx && *action) ((char *)action)[0] = 'X';
}

static unsigned native_index(unsigned receiver, ClassicControlOperation operation) {
    const char *classes[] = {"Form", "Alias", "ActiveMagicEffect"};
    const char *methods[] = {"RegisterForControl", "UnregisterForControl", "UnregisterForAllControls"};
    unsigned count = 0;
    const SkseNative *table = skse_native_table(&count);
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(table[i].class_name, classes[receiver]) && !strcmp(table[i].name, methods[operation])) return i;
    assert(!"Expected the named Classic Papyrus native");
    return count;
}

static bool invoke(unsigned receiver, ClassicControlOperation op, unsigned type, uint32_t action, bool has_base) {
    CPU cpu = {0};
    cpu.r[1] = NATIVE;
    cpu.r[4] = STACK;
    M32(STACK + 4) = has_base ? BASE : 0;
    M32(STACK + 8) = REGISTRY;
    M32(STACK + 16) = RESULT;
    M32(STACK + 20) = STATE;
    M32(NATIVE + 0x2c) = native_index(receiver, op);
    M32(STATE) = LIST;
    M32(BASE) = 16;
    M32(BASE + 4) = OBJECT;
    M64(OBJECT + 0x10) = handles[receiver];
    M32(ARGUMENT) = type;
    M32(ARGUMENT + 4) = action;
    M32(RESULT) = 0xffffffffu;
    skyrim_probe_control_native(&cpu);
    assert(cpu.r[4] == STACK + 24);
    if (cpu.r[0]) assert(M32(RESULT) == 0 && M32(RESULT + 4) == 0);
    return cpu.r[0] != 0;
}

static void use_native(unsigned receiver, ClassicControlOperation op, uint32_t action) {
    assert(invoke(receiver, op, STRING_TYPE, action, true));
}

static void send_buttons(const Button *buttons, size_t count) {
    CPU cpu = {0};
    cpu.r[4] = STACK;
    M32(STACK + 4) = EVENT_HEAD;
    M32(STACK + 8) = 0;
    M32(EVENT_HEAD) = count ? EVENTS : 0;
    assert(EVENTS + count * EVENT_SIZE <= EVENT_HEAD);
    for (size_t i = 0; i < count; ++i) {
        uint32_t event = EVENTS + (uint32_t)i * EVENT_SIZE;
        M32(event + 4) = buttons[i].device;
        M32(event + 8) = 0;
        M32(event + 0xc) = i + 1 == count ? 0 : event + EVENT_SIZE;
        M32(event + 0x10) = buttons[i].action;
        M32(event + 0x14) = buttons[i].button;
        M32(event + 0x18) = buttons[i].flags;
        MF32(event + 0x1c) = buttons[i].elapsed;
    }
    skyrim_probe_control_input(&cpu);
    assert(cpu.r[0] == 0 && cpu.r[4] == STACK + 12);
}

static void expect_event(unsigned index, unsigned recipient, uint32_t action, bool down, float elapsed) {
    assert(index < fixture.event_count);
    const QueuedEvent *event = &fixture.events[index];
    assert(event->handle == handles[recipient] && !strcmp(event->action, action_names[action_index(action)]));
    assert(!strcmp(event->name, down ? "OnControlDown" : "OnControlUp"));
    assert(event->argc == (down ? 1u : 2u) && event->held_seconds == elapsed);
    assert(event->types[0] == STRING_TYPE && event->types[1] == (down ? 0u : FLOAT_TYPE));
}

static void discard_vm_queue(void) {
    for (unsigned i = 0; i < fixture.event_count; ++i) {
        unsigned *refs = &fixture.queued_refs[handle_index(fixture.events[i].handle)];
        assert(*refs > 0);
        --*refs;
    }
    fixture.event_count = 0;
}

static void original_delete_step(CPU *cpu, unsigned step) {
    const uint32_t returns[] = {0x8d6564, 0x8d656d, 0x8d6576, 0x8d657f, 0x8d6588, 0x8d6591, 0x8d659e};
    assert(fixture.delete_calls++ == step && M32(cpu->r[4]) == returns[step]);
    assert(M64(cpu->r[4] + 4) == handles[1]);
    if (step == 3) M64(cpu->r[4] + 4) = 0; // Original is allowed to reuse its arguments.
    cpu->r[4] += 12;
}

void f_8D1E00(CPU *cpu) { original_delete_step(cpu, 0); }
void f_8D1EB0(CPU *cpu) { original_delete_step(cpu, 1); }
void f_8D11A0(CPU *cpu) { original_delete_step(cpu, 2); }
void f_8D4C40(CPU *cpu) { original_delete_step(cpu, 3); }
void f_8D4D30(CPU *cpu) { original_delete_step(cpu, 4); }
void f_8D64E0(CPU *cpu) { original_delete_step(cpu, 5); }
void f_8AF310(CPU *cpu) { original_delete_step(cpu, 6); }

void skyrim_probe_form_delete(CPU *cpu, uint32_t vm, uint64_t handle) {
    assert(fixture.delete_calls == 4 && cpu->r[4] == STACK - 12 && handle == handles[1]);
    if (vm == VM) classic_control_unregister_all(&fixture.bridge, handle);
}

static void delete_form(void (*route)(CPU *), uint32_t vm) {
    CPU cpu = {0};
    cpu.r[1] = vm;
    cpu.r[3] = 11;
    cpu.r[4] = STACK;
    cpu.r[6] = 22;
    cpu.r[7] = 33;
    M64(STACK + 4) = handles[1];
    fixture.delete_calls = 0;
    route(&cpu);
    assert(fixture.delete_calls == 7 && cpu.r[4] == STACK + 12);
    assert(cpu.r[3] == 11 && cpu.r[6] == 22 && cpu.r[7] == 33);
}

static void test_native_lifetimes(void) {
    for (unsigned receiver = 0; receiver < 3; ++receiver) {
        const ClassicControlOperation add = CLASSIC_REGISTER_CONTROL;
        use_native(receiver, add, ACTIVATE);
        use_native(receiver, add, ACTIVATE);
        use_native(receiver, add, SPRINT);
        assert(fixture.handle_refs[receiver] == 2 && fixture.action_refs[0] == 1 && fixture.action_refs[1] == 1);
        use_native(receiver, CLASSIC_UNREGISTER_CONTROL, SPRINT);
        use_native(receiver, CLASSIC_UNREGISTER_CONTROL, SPRINT);
        assert(fixture.handle_refs[receiver] == 1 && fixture.action_refs[0] == 1 && !fixture.action_refs[1]);
        use_native(receiver, CLASSIC_UNREGISTER_ALL_CONTROLS, 0);
        assert(!fixture.handle_refs[receiver] && !fixture.action_refs[0] && !fixture.bridge.head);
    }
    assert(!invoke(0, CLASSIC_REGISTER_CONTROL, INTEGER_TYPE, ACTIVATE, true));
    assert(invoke(1, CLASSIC_REGISTER_CONTROL, STRING_TYPE, ACTIVATE, false));
    assert(invoke(2, CLASSIC_REGISTER_CONTROL, 0, 0, true));
    assert(!fixture.bridge.head && !fixture.event_count);
}

static void test_delivery(void) {
    use_native(1, CLASSIC_REGISTER_CONTROL, ACTIVATE);
    use_native(0, CLASSIC_REGISTER_CONTROL, ACTIVATE);
    use_native(2, CLASSIC_REGISTER_CONTROL, READY);
    use_native(1, CLASSIC_REGISTER_CONTROL, SPRINT);

    const Button input[] = {
        {GAMEPAD, BUTTON_A, ACTIVATE, 1, 0},       // Down
        {GAMEPAD, BUTTON_A, ACTIVATE, 1, .25f},    // Held sample, no notification
        {GAMEPAD, BUTTON_A, ACTIVATE, 0, .875f},   // Up includes engine time
        {KEYBOARD, KEY_E, ACTIVATE, 1, 0},         // Same action and subscribers via the keyboard
        {KEYBOARD, KEY_E, ACTIVATE, 0, .5f},
        {GAMEPAD, BUTTON_A, SPRINT, 1, 0},         // Current binding of same physical button
        {GAMEPAD, BUTTON_A, SPRINT, 0, 1.25f},
        {GAMEPAD, 0x400, READY, 1, 0},             // Invalid macro mask
        {GAMEPAD, BUTTON_A, SEPARATE_INTERN, 1, 0}, // Same bytes, different guest identity
    };
    send_buttons(input, sizeof input / sizeof input[0]);
    assert(fixture.event_count == 10 && fixture.gfx_count == 7);
    for (unsigned i = 0; i < 2; ++i) {
        expect_event(i, i, ACTIVATE, true, 0);
        expect_event(i + 2, i, ACTIVATE, false, .875f);
        expect_event(i + 4, i, ACTIVATE, true, 0);
        expect_event(i + 6, i, ACTIVATE, false, .5f);
    }
    expect_event(8, 1, SPRINT, true, 0);
    expect_event(9, 1, SPRINT, false, 1.25f);

    use_native(0, CLASSIC_UNREGISTER_CONTROL, ACTIVATE);
    assert(!fixture.handle_refs[0] && fixture.queued_refs[0] == 4);
    expect_event(0, 0, ACTIVATE, true, 0); // Removing a registration leaves previously queued VM events.
    discard_vm_queue();

    const Button down = {GAMEPAD, BUTTON_X, READY, 1, 0};
    fixture.mutate_after_gfx = true;
    send_buttons(&down, 1);
    fixture.mutate_after_gfx = false;
    assert(M8(READY) == 'X');
    expect_event(0, 2, READY, true, 0); // VM copied before the GFX remap touched the source.
    copy_text((char *)MEM + READY, 20, action_names[2]);

    use_native(2, CLASSIC_UNREGISTER_CONTROL, READY);
    use_native(0, CLASSIC_REGISTER_CONTROL, READY);
    const Button up = {GAMEPAD, BUTTON_X, READY, 0, 2.0f};
    send_buttons(&up, 1);
    assert(fixture.event_count == 2);
    expect_event(1, 0, READY, false, 2.0f); // Subscribers are resolved again on release.
    discard_vm_queue();
}

static void test_cleanup(void) {
    assert(fixture.handle_refs[1] == 2 && fixture.handle_refs[0] == 1);
    delete_form(skyrim_probe_delete_original, VM);
    delete_form(skyrim_probe_delete_adapted, OTHER_VM);
    assert(fixture.handle_refs[1] == 2);
    delete_form(skyrim_probe_delete_adapted, VM);
    assert(!fixture.handle_refs[1] && !fixture.action_refs[0] && !fixture.action_refs[1]);
    const Button events[] = {{GAMEPAD, BUTTON_A, ACTIVATE, 1, 0}, {GAMEPAD, BUTTON_X, READY, 1, 0}};
    send_buttons(events, 2);
    assert(fixture.event_count == 1 && fixture.queued_refs[0] == 1);
    expect_event(0, 0, READY, true, 0);

    // The original VM revert runs first. The authored VM clears its old world before the host drops catalog entries.
    discard_vm_queue();
    memset(fixture.handle_refs, 0, sizeof fixture.handle_refs);
    fixture.vm_active = false;
    classic_control_after_vm_revert(&fixture.bridge);
    assert(!fixture.bridge.head && !fixture.action_refs[2] && !fixture.queued_refs[0]);

    fixture.vm_active = true;
    send_buttons(events, 2);
    assert(!fixture.event_count);
    use_native(0, CLASSIC_REGISTER_CONTROL, ACTIVATE);
    send_buttons(events, 2);
    assert(fixture.event_count == 1);
    expect_event(0, 0, ACTIVATE, true, 0);
    discard_vm_queue();
    classic_control_close(&fixture.bridge);
    assert(!fixture.bridge.head && !fixture.handle_refs[0] && !fixture.action_refs[0] && !fixture.queued_refs[0]);
}

int main(void) {
    gm_space memory = {0};
    assert(gm_reserve(&memory, NULL));
    assert(gm_commit_page(&memory, (EVENTS / memory.page_size) * memory.page_size, NULL));
    assert(gm_commit_page(&memory, (POLICY_ADDRESS / memory.page_size) * memory.page_size, NULL));
    assert(gm_commit_page(&memory, (DELETE_GLOBAL / memory.page_size) * memory.page_size, NULL));
    MEM = memory.base;
    M32(POLICY_ADDRESS) = POLICY;
    fixture.bridge.vm = (ClassicControlVm){&fixture, reference_handle, reference_action, enqueue};
    fixture.vm_active = true;
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        copy_text((char *)MEM + actions[i], 20, action_names[i]);

    test_native_lifetimes();
    test_delivery();
    test_cleanup();

    assert(gm_release(&memory, NULL));
    puts("Classic control registrations, typed FIFO VM delivery, and original form-delete ordering passed.");
    return 0;
}
