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
_Static_assert(((PC_GUEST_MOD_HOOKS_COMPILED) & (PC_GUEST_MOD_HOOK_SAFETY_LOAD | PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE))
               != PC_GUEST_MOD_HOOK_SAFETY_LOAD,
               "Safety Load requires the original menu transitions");
_Static_assert(!(PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOK_FRESH_LOAD) ||
               (PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED) == PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED,
               "Fresh Load requires the existing Save and VM stages");
_Static_assert((PC_GUEST_MOD_HOOKS_COMPILED & (PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE | PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE))
               != PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE,
               "ECE nose observation requires the TRI read observation");
_Static_assert((PC_GUEST_MOD_HOOKS_COMPILED & (PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE))
               != PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE,
               "HighHeels PEX observation requires the current VM generation");
_Static_assert((PC_GUEST_MOD_HOOKS_COMPILED & (PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE | PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE))
               != PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE,
               "HighHeels effect observation requires PEX observation");
_Static_assert(!(PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) ||
               (PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOK_HIGHHEEL_TRANSFORM_REQUIRED) ==
               PC_GUEST_MOD_HOOK_HIGHHEEL_TRANSFORM_REQUIRED,
               "HighHeels local transforms require the scene and effect observers");

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
static PcGuestModSafetyLoadHooks safety_load;
static PcGuestModSaveHooks save;
static PcGuestModCameraRenderHooks camera_render;
static PcGuestModEceTriHooks ece_tri;
static PcGuestModFreshLoadHooks fresh_load;
static PcGuestModCameraCullHooks camera_cull;
static PcGuestModEceNoseHooks ece_nose;
static PcGuestModHighHeelPexHooks highheel_pex;
static PcGuestModHighHeelSceneHooks highheel_scene;
static PcGuestModHighHeelEffectHooks highheel_effect;
static PcGuestModHighHeelTransformHooks highheel_transform;
unsigned skyrim_highheel_scene_guest_state;
bool skyrim_highheel_transform_guest_selected;
bool skyrim_highheel_pex_guest_selected;
bool skyrim_highheel_effect_guest_selected;
bool skyrim_ece_nose_guest_selected;
bool skyrim_camera_cull_guest_selected;
unsigned skyrim_camera_cull_guest_frames;

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
    if (table->groups & PC_GUEST_MOD_HOOK_SAFETY_LOAD) safety_load = table->safety_load;
    if (table->groups & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) save = table->save;
    if (table->groups & PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE) camera_render = table->camera_render;
    if (table->groups & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE) ece_tri = table->ece_tri;
    if (table->groups & PC_GUEST_MOD_HOOK_FRESH_LOAD) fresh_load = table->fresh_load;
    if (table->groups & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE) {
        camera_cull = table->camera_cull;
        skyrim_camera_cull_guest_selected = true;
    }
    if (table->groups & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE) {
        ece_nose = table->ece_nose;
        skyrim_ece_nose_guest_selected = true;
    }
    if (table->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE) {
        highheel_pex = table->highheel_pex;
        skyrim_highheel_pex_guest_selected = true;
    }
    if (table->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) {
        highheel_scene = table->highheel_scene;
        skyrim_highheel_scene_guest_state = SkyrimHighHeelSceneReady;
    }
    if (table->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE) {
        highheel_effect = table->highheel_effect;
        skyrim_highheel_effect_guest_selected = true;
    }
    if (table->groups & PC_GUEST_MOD_HOOK_HIGHHEEL_LOCAL_TRANSFORM) {
        highheel_transform = table->highheel_transform;
        skyrim_highheel_transform_guest_selected = true;
    }
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

static const PcGuestModHooksGuest observed_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_OBSERVED,
    .table_bytes = PC_GUEST_MOD_HOOKS_OBSERVED_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest safety_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_SAFETY,
    .table_bytes = PC_GUEST_MOD_HOOKS_SAFETY_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest save_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_SAVE,
    .table_bytes = PC_GUEST_MOD_HOOKS_SAVE_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest camera_render_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_CAMERA_RENDER,
    .table_bytes = PC_GUEST_MOD_HOOKS_CAMERA_RENDER_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest ece_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_ECE_TRI,
    .table_bytes = PC_GUEST_MOD_HOOKS_ECE_TRI_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest fresh_load_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_FRESH_LOAD,
    .table_bytes = PC_GUEST_MOD_HOOKS_FRESH_LOAD_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest camera_cull_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_CAMERA_CULL,
    .table_bytes = PC_GUEST_MOD_HOOKS_CAMERA_CULL_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest ece_nose_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_ECE_NOSE,
    .table_bytes = PC_GUEST_MOD_HOOKS_ECE_NOSE_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest highheel_pex_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX,
    .table_bytes = PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest highheel_scene_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE,
    .table_bytes = PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE_BYTES, .select = select_hooks
};

static const PcGuestModHooksGuest highheel_effect_reply = {
    .version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof(PcGuestModHooksGuest),
    .compiled_groups = PC_GUEST_MOD_HOOKS_COMPILED & PC_GUEST_MOD_HOOKS_HIGHHEEL_EFFECT,
    .table_bytes = PC_GUEST_MOD_HOOKS_HIGHHEEL_EFFECT_BYTES, .select = select_hooks
};

__attribute__((visibility("default")))
const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t version, uint32_t table_bytes) {
    if (bound_bytes || version != PC_GUEST_MOD_HOOKS_ABI || !pc_guest_mod_hooks_groups_for_bytes(table_bytes)) return 0;
    bound_bytes = table_bytes;
    if (table_bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES) return &original_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_OBSERVED_BYTES) return &observed_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_SAFETY_BYTES) return &safety_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_SAVE_BYTES) return &save_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_CAMERA_RENDER_BYTES) return &camera_render_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_ECE_TRI_BYTES) return &ece_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_FRESH_LOAD_BYTES) return &fresh_load_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_CAMERA_CULL_BYTES) return &camera_cull_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_ECE_NOSE_BYTES) return &ece_nose_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX_BYTES) return &highheel_pex_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE_BYTES) return &highheel_scene_reply;
    if (table_bytes == PC_GUEST_MOD_HOOKS_HIGHHEEL_EFFECT_BYTES) return &highheel_effect_reply;
    return &reply;
}

bool skyrim_highheel_pex_begin(const struct CPU *cpu, uint32_t name, uint32_t loader) {
    return skyrim_highheel_pex_guest_selected && highheel_pex.begin(cpu, name, loader);
}

uint64_t skyrim_highheel_pex_end(const struct CPU *cpu, bool parsed) {
    return skyrim_highheel_pex_guest_selected ? highheel_pex.end(cpu, parsed) : 0;
}

void skyrim_highheel_pex_linked(const struct CPU *cpu, uint64_t epoch, bool linked) {
    if (skyrim_highheel_pex_guest_selected) highheel_pex.linked(cpu, epoch, linked);
}

void skyrim_highheel_effect_plugins_ready(const struct CPU *cpu, uint32_t handler) {
    if (skyrim_highheel_effect_guest_selected) highheel_effect.plugins_ready(cpu, handler);
}

void skyrim_highheel_effect_phase(const struct CPU *cpu, uint32_t effect, uint32_t arguments, uint32_t site) {
    if (skyrim_highheel_effect_guest_selected) highheel_effect.phase(cpu, effect, arguments, site);
}

static _Thread_local struct {
    const struct CPU *cpu;
    uint32_t fs;
    unsigned receipt;
} highheel_frame;
static _Thread_local uint32_t highheel_frame_serial;

void skyrim_highheel_scene_guest_frame_begin(const struct CPU *cpu, uint32_t fs) {
    if (!cpu || !fs) return;
    if (highheel_frame.cpu) {
        highheel_frame.receipt = SkyrimHighHeelFrameAbsent;
        return;
    }
    highheel_frame.cpu = cpu;
    highheel_frame.fs = fs;
    highheel_frame.receipt = SkyrimHighHeelFrameObserved;
    if (!++highheel_frame_serial) ++highheel_frame_serial;
}

unsigned skyrim_highheel_scene_guest_frame_receipt(const struct CPU *cpu, uint32_t fs) {
    if (!cpu || !fs || cpu != highheel_frame.cpu || fs != highheel_frame.fs) return SkyrimHighHeelFrameAbsent;
    return highheel_frame.receipt;
}

uint32_t skyrim_highheel_scene_guest_frame_serial(const struct CPU *cpu, uint32_t fs) {
    return skyrim_highheel_scene_guest_frame_receipt(cpu, fs) ? highheel_frame_serial : 0;
}

void skyrim_highheel_scene_guest_frame_wait(const struct CPU *cpu, uint32_t fs, uint32_t wait_result) {
    if (skyrim_highheel_scene_guest_frame_receipt(cpu, fs) == SkyrimHighHeelFrameObserved && wait_result == 0)
        highheel_frame.receipt = SkyrimHighHeelFrameWaited;
}

void skyrim_highheel_scene_guest_frame_end(const struct CPU *cpu) {
    if (cpu == highheel_frame.cpu) {
        highheel_frame.cpu = NULL;
        highheel_frame.fs = 0;
        highheel_frame.receipt = SkyrimHighHeelFrameAbsent;
    }
}

bool skyrim_highheel_scene_guest_notify(const struct CPU *cpu, enum SkyrimHighHeelSceneNotice site,
                                        uint32_t first, uint32_t second, uint32_t third) {
    if (site == SkyrimHighHeelSceneArm) {
        unsigned ready = SkyrimHighHeelSceneReady;
        if (!__atomic_compare_exchange_n(&skyrim_highheel_scene_guest_state, &ready, SkyrimHighHeelSceneActive,
                                         false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return false;
        if (highheel_scene.notify(cpu, site, first, second, third)) return true;
        __atomic_store_n(&skyrim_highheel_scene_guest_state, SkyrimHighHeelSceneReady, __ATOMIC_RELAXED);
        return false;
    }
    if (__atomic_load_n(&skyrim_highheel_scene_guest_state, __ATOMIC_RELAXED) != SkyrimHighHeelSceneActive)
        return false;
    const bool accepted = highheel_scene.notify(cpu, site, first, second, third);
    if (site == SkyrimHighHeelSceneFinish && accepted) {
        const bool repeat = skyrim_highheel_transform_guest_selected &&
                            highheel_scene.notify(cpu, SkyrimHighHeelSceneContinue, 0, 0, 0);
        __atomic_store_n(&skyrim_highheel_scene_guest_state, repeat ? SkyrimHighHeelSceneReady : 0, __ATOMIC_RELAXED);
    }
    return accepted;
}

bool skyrim_highheel_transform_guest_local_z(const struct CPU *cpu, uint32_t node, uint32_t parent,
                                           uint32_t local_z, uint32_t *adjusted_z) {
    if (!skyrim_highheel_transform_guest_selected || !cpu || !node || !parent || !adjusted_z ||
        __atomic_load_n(&skyrim_highheel_scene_guest_state, __ATOMIC_RELAXED) != SkyrimHighHeelSceneActive ||
        (local_z & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) return false;
    uint32_t value = local_z;
    if (!highheel_transform.local_z(cpu, node, parent, local_z, &value) || value == local_z ||
        (value & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) return false;
    *adjusted_z = value;
    return true;
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

void skyrim_camera_observe_render(uint32_t frame, uint32_t first, uint32_t second) {
    if (selected & PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE) camera_render.observe(frame, first, second);
}

bool skyrim_camera_cull_guest_notify(enum SkyrimCameraCullNotice site, uint32_t first, uint32_t second, unsigned third) {
    if (!skyrim_camera_cull_guest_selected) return false;
    if (site == SkyrimCameraCullFinish) __atomic_fetch_sub(&skyrim_camera_cull_guest_frames, 1, __ATOMIC_RELAXED);
    const bool accepted = camera_cull.notify(site, first, second, third);
    if (site == SkyrimCameraCullPrepare && accepted)
        __atomic_fetch_add(&skyrim_camera_cull_guest_frames, 1, __ATOMIC_RELAXED);
    return accepted;
}

uint64_t skyrim_ece_tri_begin(const struct CPU *cpu, uint32_t path, uint32_t stream) {
    return selected & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE ? ece_tri.begin(cpu, path, stream) : 0;
}

void skyrim_ece_tri_finish(const struct CPU *cpu, uint64_t ticket, uint8_t parsed) {
    if ((selected & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE) && ticket) ece_tri.finish(cpu, ticket, parsed);
}

uint64_t skyrim_ece_nose_guest_notify(const struct CPU *cpu, enum SkyrimEceNoseNotice site, uint32_t first,
                                     uint32_t second, uint32_t third, uint32_t fourth, uint64_t receipt) {
    return skyrim_ece_nose_guest_selected ? ece_nose.notify(cpu, site, first, second, third, fourth, receipt) : 0;
}

void skyrim_loading_menu_observe(uint32_t site, uint32_t dispatcher, uint32_t event) {
    if (selected & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) loading_menu.observe(site, dispatcher, event);
}

bool skyrim_inventory_fit_enabled(void) { return (selected & PC_GUEST_MOD_HOOK_INVENTORY_FIT) != 0; }

bool skyrim_safety_load_bypass_reclamation(void) {
    return (selected & PC_GUEST_MOD_HOOK_SAFETY_LOAD) && safety_load.bypass_reclamation();
}

uint64_t skyrim_save_notify_begin(const struct CPU *cpu, uint32_t stack, uint32_t temporary_path) {
    return (selected & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) ? save.begin(cpu, stack, temporary_path) : 0;
}

uint64_t skyrim_save_notify_current(const struct CPU *cpu, unsigned phase) {
    return (selected & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) ? save.current(cpu, phase) : 0;
}

void skyrim_save_notify_event(const struct CPU *cpu, uint64_t attempt, unsigned site, int succeeded) {
    if ((selected & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) && attempt) save.event(cpu, attempt, site, succeeded);
}

uint64_t skyrim_load_notify(const struct CPU *cpu, unsigned site, uint64_t ticket, uint32_t first, uint32_t second) {
    return (selected & PC_GUEST_MOD_HOOK_FRESH_LOAD) ? fresh_load.notify(cpu, site, ticket, first, second) : 0;
}
