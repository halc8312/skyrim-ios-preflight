/* Experimental classic Papyrus support in the public iPhone host. */
#define _POSIX_C_SOURCE 200809L
#include "PcGuestSkseABI.h"
#include "PcGuestSkseTrial.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* The pinned runtime implements these four public entry points. */
extern void f_8F9720(CPU *);
extern void f_A60200(CPU *);
extern void f_842140(CPU *);
extern void f_873D70(CPU *);

enum { PAPYRUS_HOOK = 1, MENU_HOOK = 2 };
static pthread_t startup_thread;
static atomic_uint observed_hooks;

/* A returned hook is evidence of an actual call and its thread. It carries
 * no CPU pointer and does not synthesize an SKSE lifecycle notification. */
static void note_first_return(unsigned hook) {
    if (atomic_load_explicit(&observed_hooks, memory_order_relaxed) & hook) return;
    const unsigned previous = atomic_fetch_or_explicit(&observed_hooks, hook, memory_order_relaxed);
    if (previous & hook) return;
    fprintf(stderr, "IOS_CLASSIC_HOOK point=%u order=%u startup_thread=%u\n", hook,
            previous ? 2u : 1u, pthread_equal(pthread_self(), startup_thread) ? 1u : 0u);
}

static void register_papyrus_extended(CPU *cpu) {
    f_8F9720(cpu);
    note_first_return(PAPYRUS_HOOK);
}

static void create_menu_extended(CPU *cpu) {
    f_A60200(cpu);
    note_first_return(MENU_HOOK);
}

static const PcGuestSkseCallbacks extended = {
    .version = PC_GUEST_SKSE_ABI,
    .bytes = sizeof(PcGuestSkseCallbacks),
    .register_papyrus = register_papyrus_extended,
    .create_menu = create_menu_extended,
    .inventory_entry = f_842140,
    .inventory_form = f_873D70
};
static const PcGuestSkseCallbacks *originals;
static const PcGuestSkseCallbacks *selected;
static int selection_final;
int pc_guest_classic_skse_active;

/* The v1 guest copies its table once, before the profile's plugin list has
 * been published. Keep these entry points stable across the startup choice. */
static void register_papyrus(CPU *cpu) { selected->register_papyrus(cpu); }
static void create_menu(CPU *cpu) { selected->create_menu(cpu); }
static void inventory_entry(CPU *cpu) { selected->inventory_entry(cpu); }
static void inventory_form(CPU *cpu) { selected->inventory_form(cpu); }

static const PcGuestSkseCallbacks dispatch = {
    .version = PC_GUEST_SKSE_ABI, .bytes = sizeof(PcGuestSkseCallbacks),
    .register_papyrus = register_papyrus, .create_menu = create_menu,
    .inventory_entry = inventory_entry, .inventory_form = inventory_form
};

int pc_guest_bind_classic_skse(void *library) {
    if (!library || originals) {
        fputs("PRIVATE_GUEST invalid or repeated classic guest binding\n", stderr);
        return -1;
    }
    PcGuestBindClassic bind = (PcGuestBindClassic)dlsym(library, PC_GUEST_SKSE_BIND_SYMBOL);
    if (!bind) {
        fputs("PRIVATE_GUEST missing classic guest binding\n", stderr);
        return -1;
    }
    const PcGuestSkseCallbacks *callbacks = bind(&dispatch);
    if (!pc_guest_skse_callbacks_valid(callbacks)) {
        fputs("PRIVATE_GUEST incompatible classic guest callbacks\n", stderr);
        return -1;
    }
    /* Both tables outlive the run. Nothing can dispatch guest code until
     * pc_guest_open has accepted both tables and published the module. */
    selected = originals = callbacks;
    return 0;
}

int pc_guest_select_classic_skse(int enabled) {
    if (!originals || selection_final || (enabled != 0 && enabled != 1)) return -1;
    const char *value = enabled ? "1" : "0";
    if (setenv("LB_SKSE_SKYUI_PROBE", value, 1) || setenv("LB_SKSE_SELFTEST", value, 1)) {
        perror("PRIVATE_GUEST setting Classic trial options");
        unsetenv("LB_SKSE_SKYUI_PROBE");
        unsetenv("LB_SKSE_SELFTEST");
        return -1;
    }
    if (enabled) startup_thread = pthread_self();
    selected = enabled ? &extended : originals;
    pc_guest_classic_skse_active = enabled;
    selection_final = 1;
    return 0;
}

void f_8F9720_guest(CPU *cpu) { originals->register_papyrus(cpu); }
void f_A60200_guest(CPU *cpu) { originals->create_menu(cpu); }
void f_842140_guest(CPU *cpu) { originals->inventory_entry(cpu); }
void f_873D70_guest(CPU *cpu) { originals->inventory_form(cpu); }
