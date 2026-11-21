/* Authored guest and host fixtures for the optional MOD hook interface. */
#define _POSIX_C_SOURCE 200809L
#include "PcGuestSkseABI.h"
#include "PcGuestModHooksABI.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#ifdef MOD_FIXTURE_GUEST
static void base_entry(CPU *c) { c->r[0] += 5; }
const FnEntry g_fn_table_exe[] = {{1, base_entry}};
const unsigned g_fn_count_exe = 1;
void f_8F9720_guest(CPU *c) { (void)c; }
void f_A60200_guest(CPU *c) { (void)c; }
void f_842140_guest(CPU *c) { (void)c; }
void f_873D70_guest(CPU *c) { (void)c; }

#ifdef MOD_FIXTURE_BAD_REPLY
/* A guest that answers with a different table size. */
static int unused_select(const PcGuestModHooks *t) { (void)t; abort(); }
__attribute__((visibility("default")))
const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t version, uint32_t table_bytes) {
    (void)version; (void)table_bytes;
    static const PcGuestModHooksGuest reply = {PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooksGuest),
                                               1, sizeof(PcGuestModHooks) + 4, unused_select};
    return &reply;
}
#endif

#ifdef MOD_FIXTURE_ORIGINAL_TABLE
static int original_select(const PcGuestModHooks *t) {
    assert(t->version == PC_GUEST_MOD_HOOKS_ABI && t->bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES);
    assert(t->groups == PC_GUEST_MOD_HOOK_CAMERA_OBSERVE && t->camera.observe_return);
    t->camera.observe_return(0x123456);
    return 0;
}
__attribute__((visibility("default")))
const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t version, uint32_t table_bytes) {
    static const PcGuestModHooksGuest reply = {
        PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooksGuest), PC_GUEST_MOD_HOOK_CAMERA_OBSERVE,
        PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES, original_select
    };
    return version == PC_GUEST_MOD_HOOKS_ABI && table_bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES ? &reply : NULL;
}
#endif

#ifndef MOD_FIXTURE_NO_HOOKS
/* What the staged units do at each kind of call site. */
__attribute__((visibility("default"))) unsigned exercise_mod_hooks(unsigned expected) {
    unsigned seen = 0;
    skyrim_fuz_host_forget_response(7);
    SkyrimFuzLiftChoice chosen = {0};
    skyrim_fuz_host_choose_menu_voice(NULL, 0x1000, 7, &chosen);
    if (chosen.force_subtitle && !strcmp(chosen.assigned_path, "stock")) seen |= 1u << 0;
    skyrim_fuz_host_note_menu_assignment(7, &chosen);
    SkyrimFuzMenuScope menu = skyrim_fuz_host_capture_menu(1, 2, 3);
    if (menu.serial == 11) seen |= 1u << 1;
    if (skyrim_fuz_host_force_menu(&menu, 0x675CAA, 3)) seen |= 1u << 2;
    SkyrimFuzWorldScope world = skyrim_fuz_host_capture_world(4, 5);
    if (world.serial == 22) seen |= 1u << 3;
    if (skyrim_fuz_host_admit_world(&world, 0x4DFDDD, 6)) seen |= 1u << 4;
    skyrim_fuz_host_world_after_copy(1, 0, 2, &world);
    skyrim_fuz_host_release_chatter_title(6);
    if (skyrim_fuz_host_world_general(1, 2)) seen |= 1u << 5;
    skyrim_vm_host_before_revert(1);
    skyrim_vm_host_after_load(1);
    skyrim_vm_host_form_deleted(NULL, 0x900, 0x1234567890ull);
    if (skyrim_chargen_probe_skip_open_warm()) seen |= 1u << 6;
    float value = 0.0f;
    const bool pending = skyrim_racemenu_probe_pending();
    const bool accepted = skyrim_racemenu_probe_nose_height(1, 2, 3, -1.0f, 1.0f, &value);
    assert(pending == accepted);
    if (accepted) { assert(value == 0.5f); seen |= 1u << 7; }
    else assert(value == 0.0f);
    skyrim_racemenu_probe_did_apply();
    skyrim_camera_observe_return(0x123456);
    skyrim_loading_menu_observe(0xA5D9CF, 0x57000, 0x58000);
    if (skyrim_inventory_fit_enabled()) seen |= 1u << 8;
    assert(seen == expected);
    return seen;
}
#endif

#else /* host */
#include "PcGuestSkseTrial.h"

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
void f_8F9720(CPU *c) { (void)c; }
void f_A60200(CPU *c) { (void)c; }
void f_842140(CPU *c) { (void)c; }
void f_873D70(CPU *c) { (void)c; }

static unsigned calls[19];
static void forget(uint32_t r) { assert(r == 7); ++calls[0]; }
static void choose(const struct CPU *c, uint32_t slot, uint32_t r, SkyrimFuzLiftChoice *out) {
    assert(!c && slot == 0x1000 && r == 7);
    strcpy(out->assigned_path, "stock");
    out->force_subtitle = true;
    ++calls[1];
}
static void note(uint32_t r, const SkyrimFuzLiftChoice *ch) { assert(r == 7 && ch->force_subtitle); ++calls[2]; }
static SkyrimFuzMenuScope capture(uint32_t m, uint32_t n, uint32_t t) {
    assert(m == 1 && n == 2 && t == 3); ++calls[3];
    return (SkyrimFuzMenuScope){.manager = m, .serial = 11};
}
static bool force(const SkyrimFuzMenuScope *s, uint32_t ra, uint32_t t) {
    assert(s->serial == 11 && ra == 0x675CAA && t == 3); ++calls[4]; return true;
}
static SkyrimFuzWorldScope capture_world(uint32_t r, uint32_t v) {
    assert(r == 4 && v == 5); ++calls[5]; return (SkyrimFuzWorldScope){.response = r, .serial = 22};
}
static bool admit(const SkyrimFuzWorldScope *s, uint32_t ra, uint32_t t) {
    assert(s->serial == 22 && ra == 0x4DFDDD && t == 6); ++calls[6]; return true;
}
static void after_copy(uint32_t a, uint32_t i, uint32_t s, const SkyrimFuzWorldScope *w) {
    assert(a == 1 && !i && s == 2 && w->serial == 22); ++calls[7];
}
static void release_title(uint32_t t) { assert(t == 6); ++calls[8]; }
static bool general(uint32_t a, uint32_t r) { assert(a == 1 && r == 2); ++calls[9]; return true; }
static void before_revert(unsigned site) { assert(site == 1); ++calls[10]; }
static void after_load(int ok) { assert(ok == 1); ++calls[11]; }
static void form_deleted(struct CPU *c, uint32_t vm, uint64_t handle) {
    assert(!c && vm == 0x900 && handle == 0x1234567890ull); ++calls[13];
}
static bool skip_warm(void) { ++calls[12]; return true; }

static bool slider_pending(void) { ++calls[14]; return true; }
static bool slider_nose(uint32_t m, uint32_t a, uint32_t b, float min, float max, float *value) {
    assert(m == 1 && a == 2 && b == 3 && min == -1.0f && max == 1.0f);
    ++calls[15];
    *value = 0.5f;
    return true;
}
static void slider_applied(void) { ++calls[16]; }
static void camera_observed(uint32_t receiver) { assert(receiver == 0x123456); ++calls[17]; }
static void loading_observed(uint32_t site, uint32_t dispatcher, uint32_t event) {
    assert(site == 0xA5D9CF && dispatcher == 0x57000 && event == 0x58000);
    ++calls[18];
}

static PcGuestModHooks full_table(uint32_t groups) {
    return (PcGuestModHooks){
        PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooks), groups, 0,
        {forget, choose, note, capture, force},
        {capture_world, admit, after_copy, release_title, general},
        {before_revert, after_load, form_deleted}, {skip_warm}, {slider_pending, slider_nose, slider_applied},
        {camera_observed}, {loading_observed}};
}

static unsigned expected_seen(uint32_t groups) {
    unsigned seen = 0;
    if (groups & PC_GUEST_MOD_HOOK_FUZ_MENU) seen |= 7u;
    if (groups & PC_GUEST_MOD_HOOK_FUZ_WORLD) seen |= 7u << 3;
    if (groups & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE) seen |= 1u << 6;
    if (groups & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) seen |= 1u << 7;
    if (groups & PC_GUEST_MOD_HOOK_INVENTORY_FIT) seen |= 1u << 8;
    return seen;
}

static void check_calls(uint32_t groups, unsigned rounds) {
    const unsigned menu = groups & PC_GUEST_MOD_HOOK_FUZ_MENU ? rounds : 0;
    const unsigned world = groups & PC_GUEST_MOD_HOOK_FUZ_WORLD ? rounds : 0;
    const unsigned vm = groups & PC_GUEST_MOD_HOOK_VM_LIFECYCLE ? rounds : 0;
    const unsigned chargen = groups & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE ? rounds : 0;
    for (int n = 0; n < 5; ++n) assert(calls[n] == menu);
    for (int n = 5; n < 10; ++n) assert(calls[n] == world);
    assert(calls[10] == vm && calls[11] == vm && calls[13] == vm && calls[12] == chargen);
    const unsigned slider = groups & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER ? rounds : 0;
    assert(calls[14] == slider && calls[15] == slider && calls[16] == slider);
    assert(calls[17] == (groups & PC_GUEST_MOD_HOOK_CAMERA_OBSERVE ? rounds : 0));
    assert(calls[18] == (groups & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE ? rounds : 0));
}

/* argv: hooked-guest compiled-groups select-groups no-hooks-guest bad-reply-guest */
int main(int argc, char **argv) {
    assert(argc == 6);
    const uint32_t compiled = (uint32_t)strtoul(argv[2], NULL, 0);
    const uint32_t choice = (uint32_t)strtoul(argv[3], NULL, 0);

    /* Selection before any guest is opened fails. */
    assert(pc_guest_select_mod_hooks(0, NULL) != 0 && !pc_guest_mod_hooks_compiled());

    /* A guest replying with another table size is rejected with no state. */
    assert(pc_guest_open(argv[5]) == -7 && !rt_module_count(0));
    assert(pc_guest_select_mod_hooks(0, NULL) != 0);

    /* A Classic guest without the symbol is accepted and reports no groups. */
    if (!strcmp(argv[3], "no-hooks")) {
        assert(pc_guest_open(argv[4]) == 0 && !pc_guest_mod_hooks_compiled());
        const PcGuestModHooks one = full_table(PC_GUEST_MOD_HOOK_FUZ_MENU);
        assert(pc_guest_select_mod_hooks(PC_GUEST_MOD_HOOK_FUZ_MENU, &one) != 0);
        assert(pc_guest_select_mod_hooks(0, NULL) == 0 && !pc_guest_mod_hooks_selected());
        assert(pc_guest_select_mod_hooks(0, NULL) != 0);
        return 0;
    }
    if (!strcmp(argv[3], "original-guest")) {
        assert(pc_guest_open(argv[1]) == 0 && pc_guest_mod_hooks_compiled() == PC_GUEST_MOD_HOOK_CAMERA_OBSERVE);
        const PcGuestModHooks camera = full_table(PC_GUEST_MOD_HOOK_CAMERA_OBSERVE);
        assert(pc_guest_select_mod_hooks(camera.groups, &camera) == 0);
        check_calls(camera.groups, 1);
        return 0;
    }

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(lib);
    PcGuestBindModHooks bind = (PcGuestBindModHooks)dlsym(lib, PC_GUEST_MOD_HOOKS_BIND_SYMBOL);
    unsigned (*exercise)(unsigned) = (unsigned (*)(unsigned))dlsym(lib, "exercise_mod_hooks");
    assert(bind && exercise);
    /* Version and size mismatches are refused without consuming the bind. */
    assert(!bind(PC_GUEST_MOD_HOOKS_ABI + 1, sizeof(PcGuestModHooks)));
    assert(!bind(PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooks) - 1));

    if (!strcmp(argv[3], "original-host")) {
        const PcGuestModHooksGuest *reply = bind(PC_GUEST_MOD_HOOKS_ABI, PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES);
        assert(reply && reply->table_bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES);
        assert(reply->compiled_groups == (compiled & PC_GUEST_MOD_HOOKS_ORIGINAL));
        PcGuestModHooks table = full_table(PC_GUEST_MOD_HOOK_CAMERA_OBSERVE);
        assert(reply->select(&table) != 0); /* The agreed size is mandatory. */
        table.bytes = PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES;
        table.groups |= PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE;
        assert(reply->select(&table) != 0);
        table.groups &= PC_GUEST_MOD_HOOKS_ORIGINAL;
        assert(reply->select(&table) == 0);
        assert(exercise(0) == 0);
        check_calls(table.groups, 1);
        return 0;
    }

    /* Unbound: every hook keeps the original path. */
    assert(exercise(0) == 0);
    check_calls(0, 0);

    assert(pc_guest_open(argv[1]) == 0 && rt_module_count(0) == 1);
    assert(pc_guest_mod_hooks_compiled() == compiled);
    /* The host bound once; a second bind from anyone is refused. */
    assert(!bind(PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooks)));
    assert(pc_guest_bind_mod_hooks(lib) != 0);

    /* Bound but not selected. */
    assert(exercise(0) == 0);
    check_calls(0, 0);

    /* Invalid selections change nothing. */
    PcGuestModHooks table = full_table(choice);
    const uint32_t absent = ~compiled & PC_GUEST_MOD_HOOKS_DEFINED;
    if (absent && pc_guest_mod_hooks_groups_valid(absent)) {
        PcGuestModHooks other = full_table(absent);
        assert(pc_guest_select_mod_hooks(absent, &other) != 0);
    }
    const uint32_t unknown_group = 512u;
    PcGuestModHooks unknown = full_table(unknown_group);
    assert(pc_guest_select_mod_hooks(unknown_group, &unknown) != 0);
    PcGuestModHooks world_only = full_table(PC_GUEST_MOD_HOOK_FUZ_WORLD);
    assert(pc_guest_select_mod_hooks(PC_GUEST_MOD_HOOK_FUZ_WORLD, &world_only) != 0);
    if (choice) {
        assert(pc_guest_select_mod_hooks(choice, NULL) != 0);
        PcGuestModHooks mismatch = full_table(choice);
        mismatch.groups = 0;
        assert(pc_guest_select_mod_hooks(choice, &mismatch) != 0);
        PcGuestModHooks wrong = table;
        ++wrong.version;
        assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        wrong = table;
        wrong.bytes -= 1;
        assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        wrong = table;
        wrong.reserved = 1;
        assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        wrong = table;
        if (choice & PC_GUEST_MOD_HOOK_FUZ_MENU) wrong.fuz_menu.force_menu = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_VM_LIFECYCLE) wrong.vm_lifecycle.form_deleted = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE) wrong.chargen_precache.skip_open_warm = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_CAMERA_OBSERVE) wrong.camera.observe_return = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) wrong.loading_menu.observe = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) wrong.chargen_slider.did_apply = NULL;
        /* Inventory fit is a guest-only capability with no host callback. */
        if (choice != PC_GUEST_MOD_HOOK_INVENTORY_FIT)
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
    } else {
        PcGuestModHooks nonempty = full_table(PC_GUEST_MOD_HOOK_FUZ_MENU);
        assert(pc_guest_select_mod_hooks(0, &nonempty) != 0);
    }
    assert(!pc_guest_mod_hooks_selected());
    assert(exercise(0) == 0);
    check_calls(0, 0);

    assert(pc_guest_select_mod_hooks(choice, choice ? &table : NULL) == 0);
    assert(pc_guest_mod_hooks_selected() == choice);
    /* The guest copied the table; later changes to the host copy do not apply. */
    table.fuz_menu.force_menu = NULL;
    table.camera.observe_return = NULL;
    table.loading_menu.observe = NULL;
    assert(exercise(expected_seen(choice)) == expected_seen(choice));
    check_calls(choice, 1);

    /* Only once. */
    const PcGuestModHooks again = full_table(compiled);
    assert(pc_guest_select_mod_hooks(compiled, &again) != 0 && pc_guest_select_mod_hooks(0, NULL) != 0);
    assert(pc_guest_mod_hooks_selected() == choice);
    uint8_t memory = 0;
    assert(pc_guest_bind_memory(&memory) == 0);
    assert(exercise(expected_seen(choice)) == expected_seen(choice));
    check_calls(choice, 2);
    dlclose(lib);
    return 0;
}
#endif
