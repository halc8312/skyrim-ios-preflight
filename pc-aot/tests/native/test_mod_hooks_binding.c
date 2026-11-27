/* Authored guest and host fixtures for the optional MOD hook interface. */
#define _POSIX_C_SOURCE 200809L
#include "PcGuestSkseABI.h"
#include "PcGuestModHooksABI.h"
#include "PcGuestMenuFocus.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

enum {
    LOCAL_NODE = 0x65000, LOCAL_PARENT = 0x64000, LOCAL_ORIGINAL = 0x40000000, LOCAL_ADJUSTED = 0x40400000,
    LOCAL_DECLINED = 0x40800000, LOCAL_INVALID_RESULT = 0x40a00000, LOCAL_SAME_RESULT = 0x40c00000
};

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

#if defined(MOD_FIXTURE_ORIGINAL_TABLE) || defined(MOD_FIXTURE_PEX_TABLE) || \
    defined(MOD_FIXTURE_SCENE_TABLE) || defined(MOD_FIXTURE_EFFECT_TABLE)
#ifdef MOD_FIXTURE_EFFECT_TABLE
#define FIXTURE_LEGACY_TABLE_BYTES PC_GUEST_MOD_HOOKS_HIGHHEEL_EFFECT_BYTES
#define FIXTURE_LEGACY_GROUPS PC_GUEST_MOD_HOOKS_HIGHHEEL_EFFECT
#elif defined(MOD_FIXTURE_SCENE_TABLE)
#define FIXTURE_LEGACY_TABLE_BYTES PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE_BYTES
#define FIXTURE_LEGACY_GROUPS PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE
#elif defined(MOD_FIXTURE_PEX_TABLE)
#define FIXTURE_LEGACY_TABLE_BYTES PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX_BYTES
#define FIXTURE_LEGACY_GROUPS PC_GUEST_MOD_HOOK_CAMERA_OBSERVE
#else
#define FIXTURE_LEGACY_TABLE_BYTES PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES
#define FIXTURE_LEGACY_GROUPS PC_GUEST_MOD_HOOK_CAMERA_OBSERVE
#endif
static int original_select(const PcGuestModHooks *t) {
    assert(t->version == PC_GUEST_MOD_HOOKS_ABI && t->bytes == FIXTURE_LEGACY_TABLE_BYTES);
#if defined(MOD_FIXTURE_SCENE_TABLE) || defined(MOD_FIXTURE_EFFECT_TABLE)
    uint32_t expected = PC_GUEST_MOD_HOOK_CAMERA_OBSERVE | PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE;
#ifdef MOD_FIXTURE_EFFECT_TABLE
    expected |= PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE |
                PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE;
    assert(t->highheel_effect.plugins_ready && t->highheel_effect.phase);
    const CPU owner = {.r = {0x75000}};
    t->highheel_effect.plugins_ready(&owner, 0x76000);
    t->highheel_effect.phase(&owner, 0x77000, 0x78000, 0x656ec5);
    t->highheel_effect.phase(&owner, 0x77000, 0x78000, 0x65710b);
    assert(t->highheel_pex.begin(NULL, 0x61000, 0x62000));
    assert(t->highheel_pex.end(NULL, true) == 11);
    t->highheel_pex.linked(NULL, 11, true);
    t->vm_lifecycle.before_revert(1);
    t->vm_lifecycle.after_load(1);
    t->vm_lifecycle.form_deleted(NULL, 0x900, 0x1234567890ull);
#endif
    assert(t->groups == expected);
    assert(t->camera.observe_return && t->highheel_scene.notify);
    assert(t->highheel_scene.notify(NULL, SkyrimHighHeelSceneArm, 0x64000, 0, 0));
    assert(t->highheel_scene.notify(NULL, SkyrimHighHeelSceneFinish, 0x64000, 0, 0));
#else
    assert(t->groups == PC_GUEST_MOD_HOOK_CAMERA_OBSERVE && t->camera.observe_return);
#endif
    t->camera.observe_return(0x123456);
    return 0;
}
__attribute__((visibility("default")))
const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t version, uint32_t table_bytes) {
    static const PcGuestModHooksGuest reply = {
        PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooksGuest), FIXTURE_LEGACY_GROUPS,
        FIXTURE_LEGACY_TABLE_BYTES, original_select
    };
    return version == PC_GUEST_MOD_HOOKS_ABI && table_bytes == FIXTURE_LEGACY_TABLE_BYTES ? &reply : NULL;
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
    skyrim_camera_observe_render(0x60000, 0x600C8, 0x600D4);
    const uint64_t ece = skyrim_ece_tri_begin(NULL, 0x61000, 0x62000);
    if (ece) seen |= 1u << 11;
    skyrim_ece_tri_finish(NULL, 0, 1);
    skyrim_ece_tri_finish(NULL, ece, 1);
    skyrim_loading_menu_observe(0xA5D9CF, 0x57000, 0x58000);
    if (skyrim_inventory_fit_enabled()) seen |= 1u << 8;
    if (skyrim_safety_load_bypass_reclamation()) seen |= 1u << 9;
    const uint64_t attempt = skyrim_save_notify_begin(NULL, 0x1000, 0x1028);
    assert(skyrim_save_notify_current(NULL, 1) == attempt);
    if (attempt) seen |= 1u << 10;
    skyrim_save_notify_event(NULL, 0, 1, 1);
    skyrim_save_notify_event(NULL, attempt, 1, 1);
    const uint64_t load = skyrim_load_notify(NULL, SkyrimFreshBegin, 0, 0, 0);
    if (load) {
        seen |= 1u << 12;
        assert(skyrim_load_notify(NULL, SkyrimFreshFinish, load, 1, 0) == 0);
    }
    assert(!skyrim_camera_cull_guest_frames);
    const bool cull = skyrim_camera_cull_guest_notify(SkyrimCameraCullPrepare, 0x60000, 0, 0);
    assert(cull == skyrim_camera_cull_guest_selected && (unsigned)cull == skyrim_camera_cull_guest_frames);
    if (cull) {
        seen |= 1u << 13;
        assert(!skyrim_camera_cull_guest_notify(SkyrimCameraCullFinish, 0, 0, 0));
        assert(!skyrim_camera_cull_guest_frames);
    }
    const uint64_t nose = skyrim_ece_nose_guest_notify(NULL, SkyrimEceNoseParse, 1, 2, 3, 4, 9);
    assert((nose != 0) == skyrim_ece_nose_guest_selected);
    if (nose) { assert(nose == 10); seen |= 1u << 14; }
    const bool heel = skyrim_highheel_pex_begin(NULL, 0x61000, 0x62000);
    assert(heel == skyrim_highheel_pex_guest_selected);
    const uint64_t epoch = skyrim_highheel_pex_end(NULL, true);
    assert(epoch == (heel ? 11 : 0));
    skyrim_highheel_pex_linked(NULL, epoch, true);
    if (heel) seen |= 1u << 15;
    const CPU effect_owner = {.r = {0x75000}};
    static bool scene_seen, transform_seen;
    uint32_t adjusted = 0x11;
    assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, LOCAL_ORIGINAL, &adjusted));
    assert(adjusted == 0x11);
    if (skyrim_highheel_scene_guest_notify(NULL, SkyrimHighHeelSceneArm, 0x64000, 0, 0)) {
        assert(!scene_seen && skyrim_highheel_scene_guest_state == SkyrimHighHeelSceneActive);
        assert(!skyrim_highheel_scene_guest_notify(NULL, SkyrimHighHeelSceneArm, 0x64000, 0, 0));
        assert(!skyrim_highheel_transform_guest_local_z(NULL, LOCAL_NODE, LOCAL_PARENT, LOCAL_ORIGINAL, &adjusted));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, 0, LOCAL_PARENT, LOCAL_ORIGINAL, &adjusted));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, 0, LOCAL_ORIGINAL, &adjusted));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, LOCAL_ORIGINAL, NULL));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, 0x7f800000, &adjusted));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, LOCAL_DECLINED, &adjusted));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, LOCAL_INVALID_RESULT, &adjusted));
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, LOCAL_SAME_RESULT, &adjusted));
        assert(adjusted == 0x11);
        transform_seen = skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT,
                                                                LOCAL_ORIGINAL, &adjusted);
        assert(adjusted == (transform_seen ? LOCAL_ADJUSTED : 0x11));
        assert(skyrim_highheel_scene_guest_notify(NULL, SkyrimHighHeelSceneFinish, 0x64000, 0, 0));
        assert(!skyrim_highheel_scene_guest_state);
        assert(!skyrim_highheel_transform_guest_local_z(&effect_owner, LOCAL_NODE, LOCAL_PARENT, LOCAL_ORIGINAL, &adjusted));
        assert(adjusted == (transform_seen ? LOCAL_ADJUSTED : 0x11));
        scene_seen = true;
    }
    if (scene_seen) seen |= 1u << 16;
    if (transform_seen) seen |= 1u << 17;
    skyrim_highheel_effect_plugins_ready(&effect_owner, 0x76000);
    skyrim_highheel_effect_phase(&effect_owner, 0x77000, 0x78000, 0x656ec5);
    skyrim_highheel_effect_phase(&effect_owner, 0x77000, 0x78000, 0x65710b);
    assert(seen == expected);
    return seen;
}
#endif

#else /* host */

static void unexpected_focus_advance(struct CPU *cpu, uint32_t menu, uint32_t view) {
    (void)cpu; (void)menu; (void)view; abort();
}
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

static unsigned calls[41];
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
static bool safety_bypass(void) { ++calls[19]; return true; }
static uint64_t save_begin(const struct CPU *c, uint32_t stack, uint32_t path) {
    assert(!c && stack == 0x1000 && path == 0x1028);
    ++calls[20];
    return 42;
}
static uint64_t save_current(const struct CPU *c, unsigned phase) {
    assert(!c && phase == 1);
    ++calls[21];
    return 42;
}
static void save_event(const struct CPU *c, uint64_t attempt, unsigned site, int succeeded) {
    assert(!c && attempt == 42 && site == 1 && succeeded);
    ++calls[22];
}

static void camera_render_observed(uint32_t frame, uint32_t first, uint32_t second) {
    assert(frame == 0x60000 && first == 0x600C8 && second == 0x600D4);
    ++calls[23];
}

static uint64_t ece_tri_begin(const struct CPU *cpu, uint32_t path, uint32_t stream) {
    assert(!cpu && path == 0x61000 && stream == 0x62000);
    ++calls[24];
    return 7;
}
static void ece_tri_finish(const struct CPU *cpu, uint64_t ticket, uint8_t parsed) {
    assert(!cpu && ticket == 7 && parsed == 1);
    ++calls[25];
}

static uint64_t fresh_notify(const struct CPU *cpu, unsigned site, uint64_t token, uint32_t first, uint32_t second) {
    assert(!cpu && !second);
    if (site == SkyrimFreshBegin) {
        assert(!token && !first);
        ++calls[26];
        return 8;
    }
    assert(site == SkyrimFreshFinish && token == 8 && first == 1);
    ++calls[27];
    return 0;
}

static bool cull_notified(enum SkyrimCameraCullNotice site, uint32_t first, uint32_t second, unsigned third) {
    assert(!second && !third);
    if (site == SkyrimCameraCullPrepare) {
        assert(first == 0x60000);
        ++calls[28];
        return true;
    }
    assert(site == SkyrimCameraCullFinish && !first);
    ++calls[29];
    return false;
}

static uint64_t ece_nose_notified(const struct CPU *cpu, enum SkyrimEceNoseNotice site, uint32_t a,
                                    uint32_t b, uint32_t c, uint32_t d, uint64_t ticket) {
    assert(!cpu && site == SkyrimEceNoseParse && a == 1 && b == 2 && c == 3 && d == 4 && ticket == 9);
    ++calls[30];
    return 10;
}

static bool heel_begin(const struct CPU *cpu, uint32_t name, uint32_t loader) {
    assert(!cpu && name == 0x61000 && loader == 0x62000);
    ++calls[31];
    return true;
}
static uint64_t heel_end(const struct CPU *cpu, bool parsed) {
    assert(!cpu && parsed);
    ++calls[32];
    return 11;
}
static void heel_linked(const struct CPU *cpu, uint64_t epoch, bool linked) {
    assert(!cpu && epoch == 11 && linked);
    ++calls[33];
}
static bool heel_scene(const struct CPU *cpu, enum SkyrimHighHeelSceneNotice event, uint32_t first,
                       uint32_t second, uint32_t third) {
    if (event == SkyrimHighHeelSceneContinue) {
        /* Sent by the guest library right after an accepted Finish while the
         * local transform is selected. It carries no arguments. This host
         * declines, so the scene stays one-shot and the state returns to zero. */
        assert(!cpu && !first && !second && !third);
        ++calls[40];
        return false;
    }
    assert(!cpu && first == 0x64000 && !second && !third);
    assert(event == SkyrimHighHeelSceneArm || event == SkyrimHighHeelSceneFinish);
    ++calls[event == SkyrimHighHeelSceneArm ? 34 : 35];
    return true;
}
static void heel_plugins_ready(const struct CPU *cpu, uint32_t handler) {
    assert(cpu && cpu->r[0] == 0x75000 && handler == 0x76000);
    ++calls[36];
}
static void heel_effect_phase(const struct CPU *cpu, uint32_t effect, uint32_t arguments, uint32_t site) {
    assert(cpu && cpu->r[0] == 0x75000 && effect == 0x77000 && arguments == 0x78000);
    assert(site == 0x656ec5 || site == 0x65710b);
    ++calls[site == 0x656ec5 ? 37 : 38];
}
static bool heel_local_z(const struct CPU *cpu, uint32_t node, uint32_t parent,
                         uint32_t local_z, uint32_t *out) {
    assert(cpu && cpu->r[0] == 0x75000 && node == LOCAL_NODE && parent == LOCAL_PARENT && out);
    ++calls[39];
    if (local_z == LOCAL_DECLINED) {
        *out = LOCAL_ADJUSTED;
        return false;
    }
    if (local_z == LOCAL_INVALID_RESULT) {
        *out = 0x7fc00000;
        return true;
    }
    if (local_z == LOCAL_SAME_RESULT) return true;
    assert(local_z == LOCAL_ORIGINAL);
    *out = LOCAL_ADJUSTED;
    return true;
}
static PcGuestModHooks full_table(uint32_t groups) {
    return (PcGuestModHooks){
        PC_GUEST_MOD_HOOKS_ABI, sizeof(PcGuestModHooks), groups, 0,
        {forget, choose, note, capture, force},
        {capture_world, admit, after_copy, release_title, general},
        {before_revert, after_load, form_deleted}, {skip_warm}, {slider_pending, slider_nose, slider_applied},
        {camera_observed}, {loading_observed}, {safety_bypass}, {save_begin, save_current, save_event},
        {camera_render_observed}, {ece_tri_begin, ece_tri_finish}, {fresh_notify}, {cull_notified}, {ece_nose_notified},
        {heel_begin, heel_end, heel_linked}, {heel_scene}, {heel_plugins_ready, heel_effect_phase}, {heel_local_z}};
}

static unsigned expected_seen(uint32_t groups) {
    unsigned seen = 0;
    if (groups & PC_GUEST_MOD_HOOK_FUZ_MENU) seen |= 7u;
    if (groups & PC_GUEST_MOD_HOOK_FUZ_WORLD) seen |= 7u << 3;
    if (groups & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE) seen |= 1u << 6;
    if (groups & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) seen |= 1u << 7;
    if (groups & PC_GUEST_MOD_HOOK_INVENTORY_FIT) seen |= 1u << 8;
    if (groups & PC_GUEST_MOD_HOOK_SAFETY_LOAD) seen |= 1u << 9;
    if (groups & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) seen |= 1u << 10;
    if (groups & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE) seen |= 1u << 11;
    if (groups & PC_GUEST_MOD_HOOK_FRESH_LOAD) seen |= 1u << 12;
    if (groups & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE) seen |= 1u << 13;
    if (groups & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE) seen |= 1u << 14;
    if (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE) seen |= 1u << 15;
    if (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) seen |= 1u << 16;
    if (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) seen |= 1u << 17;
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
    assert(calls[19] == (groups & PC_GUEST_MOD_HOOK_SAFETY_LOAD ? rounds : 0));
    for (int n = 20; n < 23; ++n) assert(calls[n] == (groups & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE ? rounds : 0));
    assert(calls[23] == (groups & PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE ? rounds : 0));
    for (int n = 26; n < 28; ++n) assert(calls[n] == (groups & PC_GUEST_MOD_HOOK_FRESH_LOAD ? rounds : 0));
    for (int n = 24; n < 26; ++n) assert(calls[n] == (groups & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE ? rounds : 0));
    for (int n = 28; n < 30; ++n) assert(calls[n] == (groups & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE ? rounds : 0));
    assert(calls[30] == (groups & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE ? rounds : 0));
    for (int n = 31; n < 34; ++n) assert(calls[n] == (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE ? rounds : 0));
    for (int n = 34; n < 36; ++n) assert(calls[n] == (rounds && (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) ? 1u : 0u));
    for (int n = 36; n < 39; ++n) assert(calls[n] == (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE ? rounds : 0));
    assert(calls[39] == (rounds && (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) ? 4u : 0u));
    /* One Continue per accepted Finish, which needs both the scene and the local transform. */
    assert(calls[40] == (rounds && (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) &&
                         (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) ? 1u : 0u));
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
    if (!strcmp(argv[3], "scene-guest") || !strcmp(argv[3], "effect-guest")) {
        const bool effect_guest = !strcmp(argv[3], "effect-guest");
        assert(pc_guest_open(argv[1]) == 0 && pc_guest_mod_hooks_compiled() == compiled);
        const uint32_t effect = PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE |
            PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE;
        const uint32_t newer = effect_guest ? PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM |
            effect | PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE : effect;
        const PcGuestModHooks unavailable = full_table(newer);
        assert(pc_guest_mod_hooks_groups_valid(unavailable.groups));
        assert(pc_guest_select_mod_hooks(unavailable.groups, &unavailable) != 0);
        const uint32_t scene = PC_GUEST_MOD_HOOK_CAMERA_OBSERVE | PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE;
        const PcGuestModHooks supported = full_table(effect_guest ? scene | effect : scene);
        assert(pc_guest_select_mod_hooks(supported.groups, &supported) == 0);
        check_calls(supported.groups, 1);
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

    if (!strcmp(argv[3], "original-host") || !strcmp(argv[3], "observer-host") ||
        !strcmp(argv[3], "safety-host") || !strcmp(argv[3], "save-host") || !strcmp(argv[3], "camera-host") ||
        !strcmp(argv[3], "ece-host") || !strcmp(argv[3], "fresh-host") ||
        !strcmp(argv[3], "cull-host") || !strcmp(argv[3], "nose-host") || !strcmp(argv[3], "heel-host") ||
        !strcmp(argv[3], "scene-host") || !strcmp(argv[3], "effect-host")) {
        const uint32_t bytes = !strcmp(argv[3], "original-host") ? PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES
            : !strcmp(argv[3], "observer-host") ? PC_GUEST_MOD_HOOKS_OBSERVED_BYTES
            : !strcmp(argv[3], "safety-host") ? PC_GUEST_MOD_HOOKS_SAFETY_BYTES
            : !strcmp(argv[3], "save-host") ? PC_GUEST_MOD_HOOKS_SAVE_BYTES
            : !strcmp(argv[3], "camera-host") ? PC_GUEST_MOD_HOOKS_CAMERA_RENDER_BYTES
            : !strcmp(argv[3], "ece-host") ? PC_GUEST_MOD_HOOKS_ECE_TRI_BYTES
            : !strcmp(argv[3], "fresh-host") ? PC_GUEST_MOD_HOOKS_FRESH_LOAD_BYTES
            : !strcmp(argv[3], "cull-host") ? PC_GUEST_MOD_HOOKS_CAMERA_CULL_BYTES
            : !strcmp(argv[3], "nose-host") ? PC_GUEST_MOD_HOOKS_ECE_NOSE_BYTES
            : !strcmp(argv[3], "heel-host") ? PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX_BYTES
            : !strcmp(argv[3], "scene-host") ? PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE_BYTES
            : PC_GUEST_MOD_HOOKS_HIGHHEEL_EFFECT_BYTES;
        const uint32_t supported = pc_guest_mod_hooks_groups_for_bytes(bytes);
        const PcGuestModHooksGuest *reply = bind(PC_GUEST_MOD_HOOKS_ABI, bytes);
        assert(reply && reply->table_bytes == bytes);
        assert(reply->compiled_groups == (compiled & supported));
        PcGuestModHooks table = full_table(PC_GUEST_MOD_HOOK_CAMERA_OBSERVE);
        assert(reply->select(&table) != 0); /* The agreed size is mandatory. */
        table.bytes = bytes;
        table.groups |= supported & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE
            ? PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM | PC_GUEST_MOD_HOOK_HIGHHEEL_TRANSFORM_REQUIRED |
              PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE
            : supported & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE ? PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE |
                PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE
            : supported & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE ? PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE
            : supported & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE ? PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE
            : supported & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE ? PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE | PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE
            : supported & PC_GUEST_MOD_HOOK_FRESH_LOAD ? PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE
            : supported & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE ? PC_GUEST_MOD_HOOK_FRESH_LOAD | PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED
            : PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE;
        assert(reply->select(&table) != 0);
        table.groups = PC_GUEST_MOD_HOOK_CAMERA_OBSERVE;
        if (supported & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE)
            table.groups |= PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE;
        if (supported & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE)
            table.groups |= PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE |
                            PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE;
        assert(reply->select(&table) == 0);
        assert(exercise(expected_seen(table.groups)) == expected_seen(table.groups));
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
    const uint32_t unknown_group = 524288u;
    PcGuestModHooks unknown = full_table(unknown_group);
    assert(pc_guest_select_mod_hooks(unknown_group, &unknown) != 0);
    PcGuestModHooks world_only = full_table(PC_GUEST_MOD_HOOK_FUZ_WORLD);
    assert(pc_guest_select_mod_hooks(PC_GUEST_MOD_HOOK_FUZ_WORLD, &world_only) != 0);
    PcGuestModHooks safety_only = full_table(PC_GUEST_MOD_HOOK_SAFETY_LOAD);
    assert(pc_guest_select_mod_hooks(PC_GUEST_MOD_HOOK_SAFETY_LOAD, &safety_only) != 0);
    PcGuestModHooks nose_only = full_table(PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE);
    assert(pc_guest_select_mod_hooks(nose_only.groups, &nose_only) != 0);
    PcGuestModHooks effect_without_pex = full_table(PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE);
    assert(!pc_guest_mod_hooks_groups_valid(effect_without_pex.groups));
    assert(pc_guest_select_mod_hooks(effect_without_pex.groups, &effect_without_pex) != 0);
    PcGuestModHooks effect_without_vm = full_table(PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE | PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE);
    assert(!pc_guest_mod_hooks_groups_valid(effect_without_vm.groups));
    assert(pc_guest_select_mod_hooks(effect_without_vm.groups, &effect_without_vm) != 0);
    const uint32_t prerequisites = PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE;
    const uint32_t transform = prerequisites | PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM |
                               PC_GUEST_MOD_HOOK_HIGHHEEL_TRANSFORM_REQUIRED;
    PcGuestModHooks missing_scene = full_table(transform & ~PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE);
    assert(!pc_guest_mod_hooks_groups_valid(missing_scene.groups));
    assert(pc_guest_select_mod_hooks(missing_scene.groups, &missing_scene) != 0);
    PcGuestModHooks missing_effect = full_table(transform & ~PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE);
    assert(!pc_guest_mod_hooks_groups_valid(missing_effect.groups));
    assert(pc_guest_select_mod_hooks(missing_effect.groups, &missing_effect) != 0);
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
        else if (choice & PC_GUEST_MOD_HOOK_SAFETY_LOAD) wrong.safety_load.bypass_reclamation = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) wrong.loading_menu.observe = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) wrong.chargen_slider.did_apply = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) wrong.save.event = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE) wrong.camera_render.observe = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE) wrong.ece_tri.finish = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE) wrong.camera_cull.notify = NULL;
        else if (choice & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) wrong.highheel_scene.notify = NULL;
        /* Inventory fit is a guest-only capability with no host callback. */
        if (choice != PC_GUEST_MOD_HOOK_INVENTORY_FIT)
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        if (choice & PC_GUEST_MOD_HOOK_FRESH_LOAD) {
            wrong = table;
            wrong.fresh_load.notify = NULL;
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        }
        if (choice & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE) {
            wrong = table;
            wrong.ece_nose.notify = NULL;
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        }
        if (choice & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE) {
            wrong = table;
            wrong.highheel_pex.linked = NULL;
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        }
        if (choice & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE) {
            wrong = table;
            wrong.highheel_effect.plugins_ready = NULL;
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
            wrong = table;
            wrong.highheel_effect.phase = NULL;
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        }
        if (choice & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) {
            wrong = table;
            wrong.highheel_transform.local_z = NULL;
            assert(pc_guest_select_mod_hooks(choice, &wrong) != 0);
        }
    } else {
        PcGuestModHooks nonempty = full_table(PC_GUEST_MOD_HOOK_FUZ_MENU);
        assert(pc_guest_select_mod_hooks(0, &nonempty) != 0);
    }
    assert(!pc_guest_mod_hooks_selected());
    assert(!pc_guest_menu_focus_available() && !pc_guest_attach_menu_focus(unexpected_focus_advance));
    assert(exercise(0) == 0);
    check_calls(0, 0);

    assert(pc_guest_select_mod_hooks(choice, choice ? &table : NULL) == 0);
    assert(pc_guest_mod_hooks_selected() == choice);
    assert(pc_guest_menu_focus_available() == ((choice & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) != 0));
    assert(!pc_guest_attach_menu_focus(NULL));
    if (pc_guest_menu_focus_available()) {
        PcGuestBindMenuFocus bind_focus = (PcGuestBindMenuFocus)dlsym(lib, PC_GUEST_MENU_FOCUS_BIND);
        assert(bind_focus && !bind_focus(PC_GUEST_MENU_FOCUS_VERSION + 1, unexpected_focus_advance));
        assert(pc_guest_attach_menu_focus(unexpected_focus_advance));
        assert(!pc_guest_attach_menu_focus(unexpected_focus_advance));
    }
    /* The guest copied the table; later changes to the host copy do not apply. */
    table.fuz_menu.force_menu = NULL;
    table.camera.observe_return = NULL;
    table.loading_menu.observe = NULL;
    table.safety_load.bypass_reclamation = NULL;
    table.save.begin = NULL;
    table.camera_render.observe = NULL;
    table.ece_tri.begin = NULL;
    table.fresh_load.notify = NULL;
    table.camera_cull.notify = NULL;
    table.ece_nose.notify = NULL;
    table.highheel_scene.notify = NULL;
    table.highheel_effect.plugins_ready = NULL;
    table.highheel_effect.phase = NULL;
    table.highheel_transform.local_z = NULL;
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
