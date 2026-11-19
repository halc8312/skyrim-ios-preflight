/* Authored fixture. Each original writes a different result into a mock CPU. */
#define _POSIX_C_SOURCE 200809L
#include "PcGuestSkseABI.h"
#include <assert.h>

#ifdef CLASSIC_FIXTURE_GUEST
void f_8F9720(CPU *);
void f_A60200(CPU *);
void f_842140(CPU *);
void f_873D70(CPU *);
void f_8F9720_guest(CPU *c) { c->value += 10; }
void f_A60200_guest(CPU *c) { c->value += 20; }
void f_842140_guest(CPU *c) { c->value += 30; }
void f_873D70_guest(CPU *c) { c->value += 40; }

__attribute__((visibility("default"))) void exercise_classic_entries(void) {
    CPU c = {0};
    f_8F9720(&c); assert(c.value == 11);
    f_A60200(&c); assert(c.value == 33);
    f_842140(&c); assert(c.value == 66);
    f_873D70(&c); assert(c.value == 110);
}
#else
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
void f_8F9720_guest(CPU *);
void f_A60200_guest(CPU *);
void f_842140_guest(CPU *);
void f_873D70_guest(CPU *);
void f_8F9720(CPU *c) { f_8F9720_guest(c); c->value += 1; }
void f_A60200(CPU *c) { f_A60200_guest(c); c->value += 2; }
void f_842140(CPU *c) { f_842140_guest(c); c->value += 3; }
void f_873D70(CPU *c) { f_873D70_guest(c); c->value += 4; }

int main(int argc, char **argv) {
    assert(argc == 2);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    PcGuestBindClassic bind = (PcGuestBindClassic)dlsym(lib, PC_GUEST_SKSE_BIND_SYMBOL);
    assert(bind);
    PcGuestSkseCallbacks callbacks = { PC_GUEST_SKSE_ABI, sizeof(PcGuestSkseCallbacks),
        f_8F9720, f_A60200, f_842140, NULL };
    assert(!bind(NULL) && !bind(&callbacks));
    callbacks.inventory_form = f_873D70;
    callbacks.version++;
    assert(!bind(&callbacks));
    callbacks.version--;
    callbacks.bytes--;
    assert(!bind(&callbacks));
    callbacks.bytes++;
    assert(setenv("LB_SKSE_SKYUI_PROBE", "1", 1) == 0);
    assert(pc_guest_bind_classic_skse(lib) != 0);
    assert(unsetenv("LB_SKSE_SKYUI_PROBE") == 0);
    assert(setenv("LB_SKSE_SELFTEST", "0", 1) == 0);
    assert(pc_guest_bind_classic_skse(lib) == 0);
    assert(getenv("LB_SKSE_SELFTEST") && !strcmp(getenv("LB_SKSE_SELFTEST"), "1"));
    void (*exercise)(void) = (void (*)(void))dlsym(lib, "exercise_classic_entries");
    assert(exercise);
    exercise();
    assert(!bind(&callbacks));
    assert(pc_guest_bind_classic_skse(lib) != 0);
    exercise();
    dlclose(lib);
    return 0;
}
#endif
