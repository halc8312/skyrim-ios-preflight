/* SPDX-License-Identifier: GPL-3.0-only
 * Guest half of the optional MOD hook interface. The staged units call the
 * names below. An unbound, unselected or unselected-group hook returns the
 * value that keeps the original game path. Selection happens once, before any
 * guest thread starts, so the copied tables are read without locks.
 */
#include "PcGuestModHooksABI.h"

#ifndef PC_GUEST_MOD_HOOKS_COMPILED
#error "Define PC_GUEST_MOD_HOOKS_COMPILED to the staged hook groups"
#endif
_Static_assert(((PC_GUEST_MOD_HOOKS_COMPILED) & ~PC_GUEST_MOD_HOOKS_DEFINED) == 0,
               "Only groups with a version 1 table can be compiled");
_Static_assert(!((PC_GUEST_MOD_HOOKS_COMPILED) & PC_GUEST_MOD_HOOK_FUZ_WORLD)
               || ((PC_GUEST_MOD_HOOKS_COMPILED) & PC_GUEST_MOD_HOOK_FUZ_MENU),
               "The world stage reuses the menu stage's response assignment");

static uint32_t selected;
static uint32_t bound_bytes;
static int selection_final;
static PcGuestModFuzMenuHooks fuz_menu;
static PcGuestModFuzWorldHooks fuz_world;
static PcGuestModVmHooks vm_lifecycle;
static PcGuestModChargenHooks chargen_precache;
static PcGuestModChargenSliderHooks chargen_slider;
static PcGuestModCameraHooks camera;
static PcGuestModLoadingMenuHooks loading_menu;

static int select_hooks(const PcGuestModHooks *table) {
    if (!bound_bytes || selection_final || !pc_guest_mod_hooks_table_valid(table) || table->bytes != bound_bytes
            || (table->groups & ~(uint32_t)(PC_GUEST_MOD_HOOKS_COMPILED))) return -1;
    if (table->groups & PC_GUEST_MOD_HOOK_FUZ_MENU) fuz_menu = table->fuz_menu;
    if (table->groups & PC_GUEST_MOD_HOOK_FUZ_WORLD) fuz_world = table->fuz_world;
    if (table->groups & PC_GUEST_MOD_HOOK_VM_LIFECYCLE) vm_lifecycle = table->vm_lifecycle;
    if (table->groups & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE) chargen_precache = table->chargen_precache;
    if (table->groups & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) chargen_slider = table->chargen_slider;
    if (table->groups & PC_GUEST_MOD_HOOK_CAMERA_OBSERVE) camera = table->camera;
    if (table->groups & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) loading_menu = table->loading_menu;
    selected = table->groups;
    selection_final = 1;
    return 0;
}

static const PcGuestModHooksGuest reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED, .table_bytes = sizeof(PcGuestModHooks),
    .select = select_hooks
};

static const PcGuestModHooksGuest original_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_ORIGINAL,
    .table_bytes = PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES, .select = select_hooks
};

__attribute__((visibility("default")))
const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t version, uint32_t table_bytes) {
    if (bound_bytes || version != PC_GUEST_MOD_HOOKS_ABI
            || (table_bytes != sizeof(PcGuestModHooks) && table_bytes != PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES)) return 0;
    bound_bytes = table_bytes;
    return table_bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES ? &original_reply : &reply;
}

/* FUZ_MENU */
void skyrim_fuz_host_forget_response(uint32_t response) {
    if (selected & PC_GUEST_MOD_HOOK_FUZ_MENU) fuz_menu.forget_response(response);
}
void skyrim_fuz_host_choose_menu_voice(const struct CPU *caller, uint32_t argument_slot, uint32_t response,
                                       SkyrimFuzLiftChoice *choice) {
    /* Unselected: the staged caller's zeroed choice stays as it is. */
    if (selected & PC_GUEST_MOD_HOOK_FUZ_MENU) fuz_menu.choose_menu_voice(caller, argument_slot, response, choice);
}
void skyrim_fuz_host_note_menu_assignment(uint32_t response, const SkyrimFuzLiftChoice *choice) {
    if (selected & PC_GUEST_MOD_HOOK_FUZ_MENU) fuz_menu.note_menu_assignment(response, choice);
}
SkyrimFuzMenuScope skyrim_fuz_host_capture_menu(uint32_t manager, uint32_t node, uint32_t queued_title) {
    if (selected & PC_GUEST_MOD_HOOK_FUZ_MENU) return fuz_menu.capture_menu(manager, node, queued_title);
    return (SkyrimFuzMenuScope){0};
}
bool skyrim_fuz_host_force_menu(const SkyrimFuzMenuScope *scope, uint32_t return_address, uint32_t queued_title) {
    return (selected & PC_GUEST_MOD_HOOK_FUZ_MENU) && fuz_menu.force_menu(scope, return_address, queued_title);
}

/* FUZ_WORLD */
SkyrimFuzWorldScope skyrim_fuz_host_capture_world(uint32_t response, uint32_t voice) {
    if (selected & PC_GUEST_MOD_HOOK_FUZ_WORLD) return fuz_world.capture_world(response, voice);
    return (SkyrimFuzWorldScope){0};
}
bool skyrim_fuz_host_admit_world(const SkyrimFuzWorldScope *scope, uint32_t return_address, uint32_t title) {
    return (selected & PC_GUEST_MOD_HOOK_FUZ_WORLD) && fuz_world.admit_world(scope, return_address, title);
}
void skyrim_fuz_host_world_after_copy(uint32_t array, uint32_t index, uint32_t source,
                                      const SkyrimFuzWorldScope *scope) {
    if (selected & PC_GUEST_MOD_HOOK_FUZ_WORLD) fuz_world.world_after_copy(array, index, source, scope);
}
void skyrim_fuz_host_release_chatter_title(uint32_t title) {
    if (selected & PC_GUEST_MOD_HOOK_FUZ_WORLD) fuz_world.release_chatter_title(title);
}
bool skyrim_fuz_host_world_general(uint32_t array, uint32_t record) {
    return (selected & PC_GUEST_MOD_HOOK_FUZ_WORLD) && fuz_world.world_general(array, record);
}

/* VM_LIFECYCLE */
void skyrim_vm_host_before_revert(unsigned site) {
    if (selected & PC_GUEST_MOD_HOOK_VM_LIFECYCLE) vm_lifecycle.before_revert(site);
}
void skyrim_vm_host_after_load(int succeeded) {
    if (selected & PC_GUEST_MOD_HOOK_VM_LIFECYCLE) vm_lifecycle.after_load(succeeded);
}
void skyrim_vm_host_form_deleted(struct CPU *cpu, uint32_t vm, uint64_t handle) {
    if (selected & PC_GUEST_MOD_HOOK_VM_LIFECYCLE) vm_lifecycle.form_deleted(cpu, vm, handle);
}

/* CHARGEN_PRECACHE: false runs the original first-open warm call. */
bool skyrim_chargen_probe_skip_open_warm(void) {
    return (selected & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE) && chargen_precache.skip_open_warm();
}

bool skyrim_racemenu_probe_pending(void) {
    return (selected & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) && chargen_slider.pending();
}
bool skyrim_racemenu_probe_nose_height(uint32_t menu, uint32_t actor, uint32_t base,
                                     float minimum, float maximum, float *value) {
    return (selected & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) &&
        chargen_slider.nose_height(menu, actor, base, minimum, maximum, value);
}
void skyrim_racemenu_probe_did_apply(void) {
    if (selected & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) chargen_slider.did_apply();
}

void skyrim_camera_observe_return(uint32_t receiver) {
    if (selected & PC_GUEST_MOD_HOOK_CAMERA_OBSERVE) camera.observe_return(receiver);
}

void skyrim_loading_menu_observe(uint32_t site, uint32_t dispatcher, uint32_t event) {
    if (selected & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) loading_menu.observe(site, dispatcher, event);
}
