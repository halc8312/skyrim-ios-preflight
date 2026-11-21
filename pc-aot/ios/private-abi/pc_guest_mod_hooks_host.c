/* SPDX-License-Identifier: GPL-3.0-only
 * Host half of the optional MOD hook interface. A Classic guest without the
 * bind symbol is accepted and reports no groups, so features that need a hook
 * show that the guest must be rebuilt.
 */
#include "PcGuestModHooksABI.h"
#include <dlfcn.h>
#include <stdio.h>

static const PcGuestModHooksGuest *guest;
static int bind_attempted, selection_final;
static uint32_t selected;

int pc_guest_bind_mod_hooks(void *library) {
    if (!library || bind_attempted) {
        fputs("PRIVATE_GUEST invalid or repeated MOD hook binding\n", stderr);
        return -1;
    }
    PcGuestBindModHooks bind = (PcGuestBindModHooks)dlsym(library, PC_GUEST_MOD_HOOKS_BIND_SYMBOL);
    if (!bind) {
        bind_attempted = 1;
        return 0;
    }
    uint32_t table_bytes = sizeof(PcGuestModHooks);
    const PcGuestModHooksGuest *reply = bind(PC_GUEST_MOD_HOOKS_ABI, table_bytes);
    if (!reply) {
        table_bytes = PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES;
        reply = bind(PC_GUEST_MOD_HOOKS_ABI, table_bytes);
    }
    if (!reply || reply->version != PC_GUEST_MOD_HOOKS_ABI || reply->bytes != sizeof(*reply)
            || reply->table_bytes != table_bytes || !reply->select
            || !pc_guest_mod_hooks_groups_valid(reply->compiled_groups)
            || (table_bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES &&
                (reply->compiled_groups & ~PC_GUEST_MOD_HOOKS_ORIGINAL))) {
        fputs("PRIVATE_GUEST incompatible MOD hook interface\n", stderr);
        return -1;
    }
    guest = reply;
    bind_attempted = 1;
    fprintf(stderr, "PRIVATE_GUEST MOD hook groups %u\n", (unsigned)reply->compiled_groups);
    return 0;
}

/* pc_guest_open rejected the library after this binding, before publishing it. */
void pc_guest_forget_mod_hooks(void) {
    if (selection_final) return;
    guest = NULL;
    bind_attempted = 0;
}

uint32_t pc_guest_mod_hooks_compiled(void) { return guest ? guest->compiled_groups : 0; }
uint32_t pc_guest_mod_hooks_selected(void) { return selected; }

int pc_guest_select_mod_hooks(uint32_t groups, const PcGuestModHooks *implementations) {
    if (!bind_attempted || selection_final) return -1;
    if (implementations && implementations->bytes != sizeof(*implementations)) return -1;
    if (!groups) {
        if (implementations && !pc_guest_mod_hooks_table_valid(implementations)) return -1;
        if (implementations && implementations->groups) return -1;
    } else if (!guest || !implementations || implementations->groups != groups
               || (groups & ~guest->compiled_groups) || !pc_guest_mod_hooks_table_valid(implementations)) {
        return -1;
    }
    if (guest) {
        PcGuestModHooks table = {.version = PC_GUEST_MOD_HOOKS_ABI};
        if (groups) table = *implementations;
        table.bytes = guest->table_bytes;
        if (guest->select(&table) != 0) return -1;
    }
    selected = groups;
    selection_final = 1;
    return 0;
}
