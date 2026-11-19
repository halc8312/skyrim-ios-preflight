/* SPDX-License-Identifier: GPL-3.0-only */
#include "PcGuestSkseABI.h"

/* These four original bodies remain in the privately compiled framework.
 * Their public names serve both translated direct calls and the function table.
 */
extern void f_8F9720_guest(CPU *);
extern void f_A60200_guest(CPU *);
extern void f_842140_guest(CPU *);
extern void f_873D70_guest(CPU *);

static const PcGuestSkseCallbacks originals = {
    .version = PC_GUEST_SKSE_ABI, .bytes = sizeof(PcGuestSkseCallbacks),
    .register_papyrus = f_8F9720_guest, .create_menu = f_A60200_guest,
    .inventory_entry = f_842140_guest, .inventory_form = f_873D70_guest
};
static PcGuestSkseCallbacks host;

__attribute__((visibility("default")))
const PcGuestSkseCallbacks *skyrim_guest_bind_classic_v1(const PcGuestSkseCallbacks *callbacks) {
    if (host.version || !pc_guest_skse_callbacks_valid(callbacks)) return NULL;
    host = *callbacks;
    return &originals;
}

int pc_guest_skse_is_bound(void) { return host.version == PC_GUEST_SKSE_ABI; }

void f_8F9720(CPU *cpu) { host.register_papyrus(cpu); }
void f_A60200(CPU *cpu) { host.create_menu(cpu); }
void f_842140(CPU *cpu) { host.inventory_entry(cpu); }
void f_873D70(CPU *cpu) { host.inventory_form(cpu); }
