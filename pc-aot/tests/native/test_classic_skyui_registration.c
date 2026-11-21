/* SPDX-License-Identifier: GPL-3.0-only */
#include "skse_runtime.h"
#include "classic_vm_epoch.h"
#include <SDL2/SDL.h>

#include <assert.h>
#include <string.h>

enum {
    POLICY_ADDRESS = 0x01BA7008, POLICY = 0x200, REGISTRY = 0x300,
    BASE = 0x400, OBJECT = 0x500, RESULT = 0x600, STACK = 0x700,
    MENU = 0x800, MENU_AGAIN = 0x820, MOD_EVENT = 0x840, MOD_EVENT_AGAIN = 0x860,
    OLD_CALLBACK = 0x880, UPDATED_CALLBACK = 0x8A0, LATEST_CALLBACK = 0x8C0, OTHER_EVENT = 0x8E0
};
static const uint64_t handle = UINT64_C(0x100000034);
uint8_t *MEM;
int g_log_level;
static CPU cpu;
static uint32_t next_block = 0x2000;
static unsigned guest_blocks, refs, deliveries;
static bool menu_open;
static unsigned rebind_operation;
static unsigned expected_argc;
static const char *expected_callback;

static uint32_t native(const char *class_name, unsigned operation, uint32_t base,
                       uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t args[4] = {a, b, c};
    unsigned count;
    const SkseNative *table = skse_native_table(&count);
    for (unsigned i = 0; i < count; ++i) {
        if ((table[i].operation & 0x7fff) != operation || strcmp(class_name, table[i].class_name))
            continue;
        assert(skse_system_run(&cpu, &table[i], base, args, REGISTRY, RESULT));
        return M32(RESULT + 4);
    }
    assert(0);
    return 0;
}

uint32_t gmalloc(uint32_t bytes) {
    const uint32_t a = next_block + 8;
    assert(bytes && bytes <= 512);
    next_block = a + ((bytes + 7) & ~7u);
    assert(next_block < 0x10000);
    M32(a - 8) = bytes;
    ++guest_blocks;
    return a;
}

void gfree(uint32_t a) {
    assert(a >= 0x2008 && a < next_block && (a & 7) == 0);
    const uint32_t bytes = M32(a - 8);
    assert(bytes && bytes <= 512 && guest_blocks);
    memset(MEM + a, 0xDD, bytes);
    M32(a - 8) = 0;
    --guest_blocks;
}

uint32_t gstrdup(const char *str) {
    const size_t size = strlen(str) + 1;
    assert(size <= 512);
    const uint32_t a = gmalloc((uint32_t)size);
    memcpy(MEM + a, str, size);
    return a;
}

static void clear_value(uint32_t value) {
    if (M32(value) == 2 && M32(value + 4))
        gfree(M32(value + 4));
    M64(value) = 0;
}

uint32_t skse_member(CPU *c, uint32_t function, uint32_t self, int argc, const uint32_t *args) {
    assert(c == &cpu);
    switch (function) {
    case 0xC328E0:
        assert(argc == 0);
        clear_value(self);
        break;
    case 0xA51210:
        assert(argc == 1 && M32(self) == 0);
        M32(self) = gstrdup(GSTR(args[0]));
        break;
    case 0xC329E0:
        assert(argc == 1);
        clear_value(self);
        M64(self) = M64(args[0]);
        if (M32(self) == 2 && M32(self + 4))
            M32(self + 4) = gstrdup(GSTR(M32(self + 4)));
        break;
    case 0xA511B0:
        assert(argc == 0 && M32(self));
        gfree(M32(self));
        M32(self) = 0;
        break;
    default:
        assert(0);
    }
    return 1;
}

uint32_t skse_virtual(CPU *c, uint32_t self, unsigned slot, int argc, const uint32_t *args) {
    assert(c == &cpu && (((uint64_t)args[1] << 32) | args[0]) == handle);
    if (self == POLICY) {
        assert(argc == 2 && (slot == 9 || slot == 10));
        if (slot == 9) ++refs;
        else { assert(refs); --refs; }
    } else {
        assert(self == REGISTRY && slot == 35 && argc == 4 && refs);
        const uint32_t packet = args[3];
        assert(expected_callback && !strcmp(GSTR(M32(args[2])), expected_callback));
        assert(M32(packet) && M32(packet + 4) == expected_argc);
        const uint32_t payload = packet + 8;
        if (expected_argc == 4) {
            assert(M32(payload) == 2 && !strcmp(GSTR(M32(payload + 4)), GSTR(MOD_EVENT)));
            assert(M32(payload + 8) == 2 && !strcmp(GSTR(M32(payload + 12)), "menu settings"));
            assert(M32(payload + 16) == 4 && M32(payload + 20) == 0x3FC00000);
            assert(M32(payload + 24) == 16 && M32(payload + 28) == OBJECT);
        } else if (!strncmp(expected_callback, "OnMenu", 6)) {
            assert(expected_argc == 1 && M32(payload) == 2 && !strcmp(GSTR(M32(payload + 4)), GSTR(MENU)));
        } else {
            assert(expected_argc == 1 && M32(payload) == 5 && M32(payload + 4) == 1);
        }
        ++deliveries;
        if (rebind_operation) {
            /* The VM can re-register while it consumes a copied delivery.
             * Use another string object with the same name to test key equality. */
            const unsigned operation = rebind_operation;
            rebind_operation = 0;
            if (operation == S_REG_MENU) native("Form", operation, BASE, MENU_AGAIN, 0, 0);
            else native("Form", operation, BASE, MOD_EVENT_AGAIN, LATEST_CALLBACK, 0);
        }
    }
    return 1;
}

uint32_t rt_thunk(ShimFn fn, const char *name) {
    assert(fn);
    return !strcmp(name, "SKSE.Event.Destroy") ? 0x10 : 0x20;
}
void rt_fatal(const char *message, ...) { (void)message; abort(); }
CPU *rt_current_cpu(void) { return &cpu; }
uint32_t skse_menu_view(const char *name) { return menu_open && !strcmp(name, "InventoryMenu") ? 1 : 0; }
void skse_input_install(CPU *c) { assert(c == &cpu); }
void skse_gfx_clean(CPU *c, uint32_t value) { (void)c; (void)value; abort(); }
int input_desktop_pad_active(void) { abort(); }
const Uint8 *platform_keys(int *count) { (void)count; static const Uint8 keys[SDL_NUM_SCANCODES]; return keys; }
uint32_t dinput_dik_of(int key) { return (uint32_t)key; }
uint32_t SDL_GetTicks(void) { return 0; }

static void expect(const char *callback, unsigned argc, unsigned rebind) {
    expected_callback = callback;
    expected_argc = argc;
    rebind_operation = rebind;
    deliveries = 0;
}

static void send_legacy(void) {
    const uint32_t text = gstrdup("menu settings");
    native("Form", S_SEND_MOD, BASE, MOD_EVENT, text, 0x3FC00000);
    gfree(text);
}

int main(void) {
    MEM = calloc(1, POLICY_ADDRESS + 4);
    assert(MEM);
    cpu.r[4] = STACK;
    M32(STACK + 12) = 1;
    M32(POLICY_ADDRESS) = POLICY;
    M32(BASE) = 16;
    M32(BASE + 4) = OBJECT;
    M64(OBJECT + 0x10) = handle;
    strcpy(GSTR(MENU), "InventoryMenu");
    strcpy(GSTR(MENU_AGAIN), "InventoryMenu");
    strcpy(GSTR(MOD_EVENT), "MCM_Changed");
    strcpy(GSTR(MOD_EVENT_AGAIN), "MCM_Changed");
    strcpy(GSTR(OLD_CALLBACK), "OldCallback");
    strcpy(GSTR(UPDATED_CALLBACK), "UpdatedCallback");
    strcpy(GSTR(LATEST_CALLBACK), "LatestCallback");
    strcpy(GSTR(OTHER_EVENT), "MCM_Unchanged");
    skyrim_vm_epoch_registered(REGISTRY);

    menu_open = true;
    native("Form", S_REG_MENU, BASE, MENU, 0, 0);
    native("Form", S_REG_MENU, BASE, MENU_AGAIN, 0, 0);
    native("Form", S_REG_MOD, BASE, MOD_EVENT, OLD_CALLBACK, 0);
    native("Form", S_REG_MOD, BASE, MOD_EVENT_AGAIN, UPDATED_CALLBACK, 0);
    native("Form", S_REG_MOD, BASE, OTHER_EVENT, OLD_CALLBACK, 0);
    assert(refs == 3);
    expect(NULL, 0, 0);
    skse_ui_pump();  // Already open at registration. Do not invent an open edge.
    assert(!deliveries);

    expect("OnMenuClose", 1, S_REG_MENU);
    menu_open = false;
    skse_ui_pump();
    skse_ui_pump();
    assert(deliveries == 1 && !rebind_operation && refs == 3);
    expect("UpdatedCallback", 4, S_REG_MOD);
    send_legacy();
    assert(deliveries == 1 && !rebind_operation && refs == 3);

    const uint32_t stale = native("ModEvent", S_MODEVENT_CREATE, 0, MOD_EVENT, 0, 0);
    assert(stale);
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    skyrim_vm_epoch_after_load(1);
    assert(refs == 3);
    expect(NULL, 0, 0);
    assert(!native("ModEvent", S_MODEVENT_SEND, 0, stale, 0, 0));
    const uint32_t current = native("ModEvent", S_MODEVENT_CREATE, 0, MOD_EVENT, 0, 0);
    assert(current);
    native("ModEvent", S_MODEVENT_PUSH_BOOL, 0, current, 1, 0);
    expect("LatestCallback", 1, 0);
    assert(native("ModEvent", S_MODEVENT_SEND, 0, current, 0, 0) && deliveries == 1);
    expect("OnMenuOpen", 1, 0);
    menu_open = true;
    skse_ui_pump();
    skse_ui_pump();
    assert(deliveries == 1 && refs == 3);

    native("Form", S_UNREG_MENUS, BASE, 0, 0, 0);
    native("Form", S_UNREG_MODS, BASE, 0, 0, 0);
    assert(!refs && guest_blocks == 1); // Process-wide event vtable is retained.
    free(MEM);
    return 0;
}
