/* Experimental classic Papyrus support in the public iPhone host. */
#define _POSIX_C_SOURCE 200809L
#include "PcGuestSkseABI.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pinned runtime implements these four public entry points. */
extern void f_8F9720(CPU *);
extern void f_A60200(CPU *);
extern void f_842140(CPU *);
extern void f_873D70(CPU *);

static const PcGuestSkseCallbacks host_callbacks = {
    .version = PC_GUEST_SKSE_ABI,
    .bytes = sizeof(PcGuestSkseCallbacks),
    .register_papyrus = f_8F9720,
    .create_menu = f_A60200,
    .inventory_entry = f_842140,
    .inventory_form = f_873D70
};
static const PcGuestSkseCallbacks *originals;

int pc_guest_bind_classic_skse(void *library) {
    const char *skyui_probe = getenv("LB_SKSE_SKYUI_PROBE");
    if (skyui_probe && !strcmp(skyui_probe, "1")) {
        fputs("PRIVATE_GUEST SkyUI version probe is disabled in the classic Papyrus experiment\n", stderr);
        return -1;
    }
    if (!library || originals) {
        fputs("PRIVATE_GUEST invalid or repeated classic guest binding\n", stderr);
        return -1;
    }
    PcGuestBindClassic bind = (PcGuestBindClassic)dlsym(library, PC_GUEST_SKSE_BIND_SYMBOL);
    if (!bind) {
        fputs("PRIVATE_GUEST missing classic guest binding\n", stderr);
        return -1;
    }
    if (setenv("LB_SKSE_SELFTEST", "1", 1) != 0) {
        perror("PRIVATE_GUEST enabling classic Papyrus self-test");
        return -1;
    }
    const PcGuestSkseCallbacks *callbacks = bind(&host_callbacks);
    if (!pc_guest_skse_callbacks_valid(callbacks)) {
        fputs("PRIVATE_GUEST incompatible classic guest callbacks\n", stderr);
        return -1;
    }
    /* Both tables outlive the run. Nothing can dispatch guest code until
     * pc_guest_open has accepted both tables and published the module. */
    originals = callbacks;
    return 0;
}

void f_8F9720_guest(CPU *cpu) { originals->register_papyrus(cpu); }
void f_A60200_guest(CPU *cpu) { originals->create_menu(cpu); }
void f_842140_guest(CPU *cpu) { originals->inventory_entry(cpu); }
void f_873D70_guest(CPU *cpu) { originals->inventory_form(cpu); }
