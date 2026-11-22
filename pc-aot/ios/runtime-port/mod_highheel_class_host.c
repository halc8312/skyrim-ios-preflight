/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_highheel_class_host.h"
#include "PcGuestABI.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern uint8_t *MEM;
extern CPU *rt_current_cpu(void);
extern uint32_t gmalloc(uint32_t size);
extern void gfree(uint32_t address);
extern uint32_t skse_member(CPU *, uint32_t address, uint32_t self, int count, const uint32_t *);

enum {
    FIRST_MAPPED = 0x10000, LINKED_FLAGS = 0x10,
    GLOBAL_FUNCTION = 0xc36540, MEMBER_FUNCTION = 0xc365a0
};

static pthread_mutex_t bindings_lock = PTHREAD_MUTEX_INITIALIZER;
static HighHeelNativeBindings bindings;
static bool ambiguous;
static atomic_uint reported;

static bool valid_address(uint64_t address, unsigned size) {
    return address >= FIRST_MAPPED && !(address & 3u) && address + size <= UINT64_C(0x100000000);
}

static uint32_t word(uint32_t address) {
    uint32_t value;
    memcpy(&value, MEM + address, sizeof value);
    return value;
}

static uint32_t lookup(CPU *cpu, uint32_t cls, uint32_t name, bool global, uint32_t table, uint32_t count) {
    if (!count) return 0;
    const uint32_t slot = skse_member(cpu, global ? GLOBAL_FUNCTION : MEMBER_FUNCTION, cls, 1, &name);
    if (slot < table || (slot & 3u) || slot - table >= count * 4) return 0;
    const uint32_t function = word(slot);
    return valid_address(function, 4) ? function : 0;
}

static bool capture(CPU *cpu, HighHeelNativeBindings *row) {
    static const char names[][11] = {"MoveUp", "OnUpdate", "MoveDown", "KeepUpdate", "StopUpdate"};
    const uint32_t cls = row->effect_class;
    const uint32_t flags = word(cls + LINKED_FLAGS), counts = word(cls + 0x14), data = word(cls + 0x1c);
    const uint32_t globals = (counts >> 20) & 0x1ffu, members = word(cls + 0x18) & 0x7ffu;
    const unsigned prefix = 10 * ((counts >> 10) & 0x3ffu) + 2 * ((flags >> 8) & 0x3ffu) +
                            ((flags >> 2) & 0x3fu) + 3 * (counts & 0x3ffu);
    const uint64_t table = (uint64_t)data + prefix * 4u;
    if ((flags & 3u) != 3 || globals > 8 || members < 3 || members > 64 ||
        !valid_address(data, 4) || !valid_address(table, (globals + members) * 4)) return false;
    const uint32_t scratch = gmalloc(sizeof names);
    if (!valid_address(scratch, sizeof names)) {
        if (scratch) gfree(scratch);
        return false;
    }
    memcpy(MEM + scratch, names, sizeof names);
    const uint32_t global_table = (uint32_t)table, member_table = global_table + globals * 4;
    row->move_up = lookup(cpu, cls, scratch, false, member_table, members);
    row->on_update = lookup(cpu, cls, scratch + sizeof names[0], false, member_table, members);
    row->move_down = lookup(cpu, cls, scratch + 2 * sizeof names[0], false, member_table, members);
    row->keep_update = lookup(cpu, cls, scratch + 3 * sizeof names[0], true, global_table, globals);
    row->stop_update = lookup(cpu, cls, scratch + 4 * sizeof names[0], true, global_table, globals);
    gfree(scratch);
    return row->move_up && row->on_update && row->move_down && row->move_up != row->on_update &&
           row->move_up != row->move_down && row->on_update != row->move_down;
}

static bool same_scope(SkyrimVmScope scope) {
    SkyrimVmScope current;
    return skyrim_vm_scope_current(&current) && current.epoch == scope.epoch && current.registry == scope.registry;
}

void skyrim_highheel_class_observe(CPU *cpu, SkyrimVmScope scope, uint32_t cls) {
    if (!MEM || !cpu || cpu != rt_current_cpu() || !valid_address(cls, 0x20) || !same_scope(scope)) return;
    const int saved_errno = errno;
    HighHeelNativeBindings row = {.epoch = scope.epoch, .registry = scope.registry, .effect_class = cls};
    bool members = capture(cpu, &row);
    if (!same_scope(scope)) { errno = saved_errno; return; }
    const bool globals = members && row.keep_update && row.stop_update && row.keep_update != row.stop_update;
    pthread_mutex_lock(&bindings_lock);
    if (members && row.epoch > bindings.epoch) {
        bindings = row;
        ambiguous = false;
    } else if (row.epoch == bindings.epoch &&
               (!members || row.registry != bindings.registry || row.effect_class != bindings.effect_class ||
                row.move_up != bindings.move_up || row.on_update != bindings.on_update ||
                row.move_down != bindings.move_down || row.keep_update != bindings.keep_update ||
                row.stop_update != bindings.stop_update)) {
        ambiguous = true;
    }
    members = members && bindings.epoch == row.epoch && !ambiguous;
    pthread_mutex_unlock(&bindings_lock);
    const unsigned bit = 1u << (2u * (unsigned)members + (unsigned)(members && globals));
    if (!(atomic_fetch_or_explicit(&reported, bit, memory_order_relaxed) & bit))
        fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_METHODS schema=1 members=%u globals=%u\n",
                (unsigned)members, (unsigned)(members && globals));
    errno = saved_errno;
}

bool skyrim_highheel_class_snapshot(SkyrimVmScope scope, HighHeelNativeBindings *out) {
    if (!out || !same_scope(scope)) return false;
    HighHeelNativeBindings copy;
    pthread_mutex_lock(&bindings_lock);
    const bool found = !ambiguous && bindings.epoch == scope.epoch && bindings.registry == scope.registry;
    if (found) copy = bindings;
    pthread_mutex_unlock(&bindings_lock);
    if (!found || !same_scope(scope)) return false;
    *out = copy;
    return true;
}
