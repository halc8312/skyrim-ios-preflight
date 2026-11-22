/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_mod_layout.h"
#include "classic_vm_epoch.h"
#include "mod_highheel_class_host.h"
#include "mod_highheel_effect_host.h"
#include "mod_highheel_pex_host.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    HANDLER = 0x11000, FILES = 0x12000, FILE_STRIDE = 0x500,
    EFFECT = 0x15000, ITEM = 0x15100, SETTING = 0x15200, ACTOR = 0x15300,
    ARGS = 0x15400, SLOT = 0x15500, ESTABLISHED_SETTING = 0x02000800,
    START = 0x656ec5, FINISH = 0x65710b,
    SCRIPT_CLASS = 0x16000, CLASS_DATA = 0x16100, CLASS_FUNCTIONS = CLASS_DATA + 19 * 4,
    STOCK_OBJECTS = 0x16500, SCRATCH = 0x17000
};

uint8_t *MEM;
static struct CPU owner, foreign;
static bool scratch_live;
static unsigned class_mode;
struct CPU *rt_current_cpu(void) { return &owner; }

uint32_t gmalloc(uint32_t size) {
    assert(!scratch_live && size && size <= 80);
    scratch_live = true;
    return SCRATCH;
}

void gfree(uint32_t address) {
    assert(scratch_live && address == SCRATCH);
    scratch_live = false;
}

uint32_t skse_member(CPU *cpu, uint32_t address, uint32_t self, int count, const uint32_t *args) {
    assert(cpu == &owner && self == SCRIPT_CLASS && count == 1 && scratch_live &&
           args[0] >= SCRATCH && args[0] < SCRATCH + 80);
    const char *name = (const char *)MEM + args[0];
    if (address == 0xc36540) {
        const bool keep = !strcmp(name, "KeepUpdate");
        assert(keep || !strcmp(name, "StopUpdate"));
        if (class_mode == 5 && keep) return CLASS_FUNCTIONS + 8;
        return CLASS_FUNCTIONS + (keep ? 0 : 4);
    }
    assert(address == 0xc365a0);
    if (!strcmp(name, "MoveUp")) return class_mode == 6 ? CLASS_FUNCTIONS : CLASS_FUNCTIONS + 8;
    if (!strcmp(name, "OnUpdate")) return CLASS_FUNCTIONS + 12;
    assert(!strcmp(name, "MoveDown"));
    return CLASS_FUNCTIONS + 16;
}

static void put(uint32_t address, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) MEM[address + n] = (uint8_t)(value >> (8u * n));
}

static bool digest(const void *bytes, size_t size, uint8_t out[32]) {
    if (size < 32) return false;
    memcpy(out, bytes, 32);
    return true;
}

static void set_plugins(unsigned count) {
    const char *const names[] = {"Skyrim.esm", "Update.esm", "hdtHighHeel.esm", "Override.esp"};
    put(CLASSIC_DATA_HANDLER, HANDLER);
    put(HANDLER + CLASSIC_LOADED_COUNT, count);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t file = FILES + i * FILE_STRIDE;
        put(HANDLER + CLASSIC_LOADED_MODS + 4 * i, file);
        strcpy((char *)MEM + file + CLASSIC_FILE_NAME, names[i]);
        MEM[file + CLASSIC_FILE_INDEX] = (uint8_t)i;
    }
}

static void send_phase(const PcGuestModHooks *table, uint32_t setting, bool starting) {
    put(SETTING + 0xc, setting);
    put(ARGS + 12, starting);
    table->highheel_effect.phase(&owner, EFFECT, ARGS, starting ? START : FINISH);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const unsigned mode = (unsigned)strtoul(argv[2], NULL, 10);
    MEM = calloc(1, CLASSIC_DATA_HANDLER + 4);
    assert(MEM);
    set_plugins(mode == 1 ? 2 : mode == 2 ? 4 : 3);
    const int dir = open(argv[1], O_RDONLY | O_DIRECTORY);
    assert(dir >= 0 && skyrim_highheel_effect_prepare(dir, digest));
    assert(close(dir) == 0);
    if (mode == 3) skyrim_highheel_effect_cancel();
    PcGuestModHooks table = {0};
    skyrim_highheel_effect_fill(&table);
    skyrim_highheel_pex_fill(&table);
    table.highheel_effect.plugins_ready(&foreign, HANDLER);
    table.highheel_effect.plugins_ready(&owner, HANDLER + 4);
    errno = EDOM;
    table.highheel_effect.plugins_ready(&owner, HANDLER);
    assert(errno == EDOM);
    table.highheel_effect.plugins_ready(&owner, HANDLER);

    put(EFFECT, 0x10c9de4);
    put(EFFECT + 0x34, ITEM);
    put(EFFECT + 0x50, 0x40e80000);
    put(EFFECT + 0x5c, 0xf223);
    put(ITEM + 0xc, SETTING);
    MEM[SETTING + 0x12] = 0x12;
    put(ACTOR + 0xc, 0x14);
    MEM[ACTOR + 0x12] = 0x3e;
    put(ARGS + 4, SLOT);
    put(ARGS + 8, 0xf223);
    put(SLOT, ACTOR);
    send_phase(&table, ESTABLISHED_SETTING, true);
    skyrim_vm_epoch_registered(1);
    const SkyrimVmScope initial = {1, 1};
    const uint64_t first_handle = UINT64_C(0x2f22300000014);
    const bool effect_enabled = mode == 0 || mode >= 4;
    assert(!skyrim_highheel_effect_matches(initial, first_handle, 0x14, true, 0x40e80000));
    send_phase(&table, 0x03000800, true);
    put(SETTING + 0xc, ESTABLISHED_SETTING);
    table.highheel_effect.phase(&foreign, EFFECT, ARGS, START);
    send_phase(&table, ESTABLISHED_SETTING, true);
    assert(skyrim_highheel_effect_matches(initial, first_handle, 0x14, true, 0x40e80000) == effect_enabled);
    assert(!skyrim_highheel_effect_matches(initial, first_handle, 0x15, true, 0x40e80000));
    assert(!skyrim_highheel_effect_matches(initial, first_handle, 0x14, true, 0x40800000));
    if (mode >= 4) {
        class_mode = mode;
        skyrim_highheel_pex_observe_class(skyrim_highheel_class_observe);
        put(SCRIPT_CLASS + 0x10, 3 | (1 << 8) | (1 << 2));
        put(SCRIPT_CLASS + 0x14, (2 << 20) | (1 << 10) | 2);
        put(SCRIPT_CLASS + 0x18, 3);
        put(SCRIPT_CLASS + 0x1c, CLASS_DATA);
        for (unsigned i = 0; i < 5; ++i) put(CLASS_FUNCTIONS + 4 * i, STOCK_OBJECTS + 4 * i);
        owner.r[3] = SCRIPT_CLASS;
    }
    table.highheel_pex.linked(&owner, 1, true);
    assert(skyrim_highheel_pex_linked_in(1));
    HighHeelNativeBindings bindings = {0};
    const bool has_bindings = skyrim_highheel_class_snapshot(initial, &bindings);
    assert(has_bindings == (mode == 4 || mode == 5 || mode == 7) && !scratch_live);
    if (has_bindings) {
        assert(bindings.epoch == 1 && bindings.registry == 1 && bindings.effect_class == SCRIPT_CLASS);
        assert(bindings.keep_update == (mode == 5 ? 0 : STOCK_OBJECTS) && bindings.stop_update == STOCK_OBJECTS + 4);
        assert(bindings.move_up == STOCK_OBJECTS + 8 && bindings.on_update == STOCK_OBJECTS + 12 &&
               bindings.move_down == STOCK_OBJECTS + 16);
    }
    if (mode == 7) {
        class_mode = 6;
        table.highheel_pex.linked(&owner, 1, true);
        assert(!skyrim_highheel_class_snapshot(initial, &bindings) && !scratch_live);
    }
    send_phase(&table, ESTABLISHED_SETTING, true);
    send_phase(&table, ESTABLISHED_SETTING, true);
    send_phase(&table, ESTABLISHED_SETTING, false);
    assert(!skyrim_highheel_effect_matches(initial, first_handle, 0x14, false, 0));
    const uint64_t second_handle = UINT64_C(0x2f22400000014);
    put(EFFECT + 0x5c, 0xf224);
    put(ARGS + 8, 0xf224);
    send_phase(&table, ESTABLISHED_SETTING, true);
    assert(skyrim_highheel_effect_matches(initial, second_handle, 0x14, true, 0x40e80000) == effect_enabled);
    assert(skyrim_highheel_effect_matches(initial, second_handle, 0x14, false, 0) == effect_enabled);
    send_phase(&table, ESTABLISHED_SETTING, false);
    assert(!skyrim_highheel_effect_matches(initial, second_handle, 0x14, true, 0x40e80000));
    assert(skyrim_highheel_effect_matches(initial, second_handle, 0x14, false, 0) == effect_enabled);
    for (unsigned n = 1; n <= 700; ++n) {
        put(EFFECT + 0x5c, n);
        put(ARGS + 8, n);
        send_phase(&table, ESTABLISHED_SETTING, true);
        send_phase(&table, ESTABLISHED_SETTING, false);
    }
    assert(skyrim_highheel_effect_matches(initial, UINT64_C(0x202bc00000014), 0x14, false, 0) == effect_enabled);
    assert(skyrim_highheel_effect_matches(initial, second_handle, 0x14, false, 0) == effect_enabled);
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    skyrim_vm_epoch_after_load(1);
    assert(!skyrim_highheel_class_snapshot(initial, &bindings));
    send_phase(&table, ESTABLISHED_SETTING, false);
    assert(!skyrim_highheel_effect_matches(initial, second_handle, 0x14, false, 0));
    assert(!skyrim_highheel_effect_matches((SkyrimVmScope){2, 1}, second_handle, 0x14, false, 0));
    skyrim_highheel_effect_cancel();
    free(MEM);
}
