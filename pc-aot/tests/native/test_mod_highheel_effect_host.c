/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_mod_layout.h"
#include "classic_vm_epoch.h"
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
    START = 0x656ec5, FINISH = 0x65710b
};

uint8_t *MEM;
static struct CPU owner, foreign;
struct CPU *rt_current_cpu(void) { return &owner; }

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
    send_phase(&table, 0x03000800, true);
    put(SETTING + 0xc, ESTABLISHED_SETTING);
    table.highheel_effect.phase(&foreign, EFFECT, ARGS, START);
    send_phase(&table, ESTABLISHED_SETTING, true);
    table.highheel_pex.linked(&owner, 1, true);
    assert(skyrim_highheel_pex_linked_in(1));
    send_phase(&table, ESTABLISHED_SETTING, true);
    send_phase(&table, ESTABLISHED_SETTING, true);
    send_phase(&table, ESTABLISHED_SETTING, false);
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    skyrim_vm_epoch_after_load(1);
    send_phase(&table, ESTABLISHED_SETTING, false);
    skyrim_highheel_effect_cancel();
    free(MEM);
}
