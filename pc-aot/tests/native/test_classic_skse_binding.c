/* Authored guest and host fixtures for the real base and Classic v1 bind paths. */
#define _POSIX_C_SOURCE 200809L
#include "PcGuestSkseABI.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef CLASSIC_FIXTURE_GUEST
static void base_entry(CPU *c) { c->r[0] += 5; }
const FnEntry g_fn_table_exe[] = {{1, base_entry}};
const unsigned g_fn_count_exe = 1;

#if defined(CLASSIC_FIXTURE_BAD)
int pc_guest_skse_is_bound(void) { return 0; }
__attribute__((visibility("default")))
const PcGuestSkseCallbacks *skyrim_guest_bind_classic_v1(const PcGuestSkseCallbacks *callbacks) {
    static const PcGuestSkseCallbacks invalid = {.version = PC_GUEST_SKSE_ABI + 1};
    assert(pc_guest_skse_callbacks_valid(callbacks));
    return &invalid;
}
#elif defined(SKYRIM_IOS_CLASSIC_SKSE)
void f_8F9720(CPU *);
void f_A60200(CPU *);
void f_842140(CPU *);
void f_873D70(CPU *);
void f_8F9720_guest(CPU *c) { c->r[0] += 10; }
void f_A60200_guest(CPU *c) { c->r[0] += 20; }
void f_842140_guest(CPU *c) { c->r[0] += 30; }
void f_873D70_guest(CPU *c) { c->r[0] += 40; }

__attribute__((visibility("default"))) void exercise_classic_entries(unsigned enabled) {
    CPU c = {0};
    f_8F9720(&c); assert(c.r[0] == 10 + enabled);
    f_A60200(&c); assert(c.r[0] == 30 + 3 * enabled);
    f_842140(&c); assert(c.r[0] == 60 + 6 * enabled);
    f_873D70(&c); assert(c.r[0] == 100 + 10 * enabled);
}
#endif

#else
/* The authored guest needs no host service beyond binding its memory. */
void rt_call(CPU *c, uint32_t a) { (void)c; (void)a; abort(); }
void rt_call_ic(CPU *c, uint32_t a, const FnEntry **cache) { (void)c; (void)a; (void)cache; abort(); }
void rt_unimpl(CPU *c, uint32_t a, const char *label) { (void)c; (void)a; (void)label; abort(); }
void rt_int3(CPU *c, uint32_t a) { (void)c; (void)a; abort(); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *d, uint32_t *e) {
    (void)c; (void)a; (void)b; (void)d; (void)e; abort();
}
uint64_t rt_rdtsc(void) { abort(); }
double f80_load(uint32_t a) { (void)a; abort(); }
void f80_store(uint32_t a, double v) { (void)a; (void)v; abort(); }
void fxam(CPU *c) { (void)c; abort(); }
void fprem(CPU *c, int ieee) { (void)c; (void)ieee; abort(); }
void fpu_env(CPU *c, uint32_t a, int op) { (void)c; (void)a; (void)op; abort(); }
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t a) { (void)c; (void)a; abort(); }
void rt_setjmp3(CPU *c) { (void)c; abort(); }

#ifdef SKYRIM_IOS_CLASSIC_SKSE
#include "PcGuestSkseTrial.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
static const char *fail_setting;
int fixture_setenv(const char *name, const char *value, int overwrite) {
    if (fail_setting && !strcmp(name, fail_setting)) {
        fail_setting = NULL; errno = ENOMEM; return -1;
    }
    return setenv(name, value, overwrite);
}
void f_8F9720_guest(CPU *);
void f_A60200_guest(CPU *);
void f_842140_guest(CPU *);
void f_873D70_guest(CPU *);
void f_8F9720(CPU *c) { f_8F9720_guest(c); ++c->r[0]; }
void f_A60200(CPU *c) { f_A60200_guest(c); c->r[0] += 2; }
void f_842140(CPU *c) { f_842140_guest(c); c->r[0] += 3; }
void f_873D70(CPU *c) { f_873D70_guest(c); c->r[0] += 4; }

static void *fixture_worker(void *context) {
    void (*exercise)(unsigned) = *(void (**)(unsigned))context;
    exercise(1);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 5);
    const int enabled = strcmp(argv[4], "off") != 0;
    assert(setenv("LB_SKSE_SKYUI_PROBE", "1", 1) == 0);
    assert(setenv("LB_SKSE_SELFTEST", "1", 1) == 0);
    assert(!pc_guest_classic_skse_active && pc_guest_select_classic_skse(1) != 0);
    assert(pc_guest_open(argv[2]) == -3);  /* ordinary v1 guest */
    assert(pc_guest_open(argv[3]) == -6);  /* malformed Classic v1 reply */
    assert(pc_guest_select_classic_skse(0) != 0 && !rt_module_count(0));

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    PcGuestBindClassic bind = (PcGuestBindClassic)dlsym(lib, PC_GUEST_SKSE_BIND_SYMBOL);
    assert(bind);
    PcGuestSkseCallbacks callbacks = {PC_GUEST_SKSE_ABI, sizeof(PcGuestSkseCallbacks),
                                      f_8F9720, f_A60200, f_842140, NULL};
    assert(!bind(NULL) && !bind(&callbacks));
    callbacks.inventory_form = f_873D70;
    ++callbacks.version;
    assert(!bind(&callbacks));
    --callbacks.version;
    --callbacks.bytes;
    assert(!bind(&callbacks));
    ++callbacks.bytes;

    assert(pc_guest_open(argv[1]) == 0 && rt_module_count(0) == 1);
    void (*exercise)(unsigned) = (void (*)(unsigned))dlsym(lib, "exercise_classic_entries");
    assert(exercise && !pc_guest_classic_skse_active);
    exercise(0);
    assert(pc_guest_select_classic_skse(-1) != 0 && pc_guest_select_classic_skse(2) != 0);
    if (!strcmp(argv[4], "fail-first")) fail_setting = "LB_SKSE_SKYUI_PROBE";
    if (!strcmp(argv[4], "fail-second")) fail_setting = "LB_SKSE_SELFTEST";
    if (fail_setting) {
        assert(pc_guest_select_classic_skse(1) != 0 && !pc_guest_classic_skse_active);
        assert(!getenv("LB_SKSE_SKYUI_PROBE") && !getenv("LB_SKSE_SELFTEST"));
        exercise(0);
    }
    assert(pc_guest_select_classic_skse(enabled) == 0);
    assert(pc_guest_classic_skse_active == enabled);
    uint8_t memory = 0;
    assert(pc_guest_bind_memory(&memory) == 0 && pc_guest_bind_memory(&memory) != 0);
    if (!strcmp(argv[4], "worker")) {
        pthread_t thread;
        assert(pthread_create(&thread, NULL, fixture_worker, &exercise) == 0);
        assert(pthread_join(thread, NULL) == 0);
    } else {
        exercise(enabled);
    }
    assert(!bind(&callbacks) && pc_guest_bind_classic_skse(lib) != 0 && pc_guest_open(argv[1]) != 0);
    assert(pc_guest_select_classic_skse(enabled) != 0 && pc_guest_select_classic_skse(!enabled) != 0);
    assert(!strcmp(getenv("LB_SKSE_SKYUI_PROBE"), enabled ? "1" : "0"));
    assert(!strcmp(getenv("LB_SKSE_SELFTEST"), enabled ? "1" : "0"));
    assert(pc_guest_classic_skse_active == enabled);
    exercise(enabled);
    dlclose(lib);
    return 0;
}
#else
int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[2], "wrong")) {
        assert(pc_guest_open(argv[1]) == -3 && !rt_module_count(0));
        return 0;
    }
    uint8_t memory = 0;
    assert(pc_guest_open(argv[1]) == 0 && pc_guest_bind_memory(&memory) == 0);
    assert(rt_module_count(0) == 1);
    CPU cpu = {0};
    g_modules[0].table[0].fn(&cpu);
    assert(cpu.r[0] == 5);
    return 0;
}
#endif
#endif
