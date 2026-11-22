/* SPDX-License-Identifier: GPL-3.0-only
 * Optional MOD hook interface between the Classic host and a guest built with
 * reviewed staged units (scripts/mod_hooks_guest.py). It is separate from the
 * Classic v1 callbacks in PcGuestSkseABI.h, which stay unchanged.
 *
 * The guest exports one bind function. The host calls it once from
 * pc_guest_open after the Classic binding and learns which hook groups the
 * guest was compiled with. Before the guest starts, the host selects groups
 * once and passes its implementations. Until then, and for groups it does not
 * select, every guest hook returns the value that keeps the original path.
 * Both halves are compiled for arm64 with Clang, so the Fuz seam structures may
 * be returned by value.
 */
#pragma once
#include "PcGuestABI.h"
#include "lift_world_seam.h" /* experiments/fuz-voice: menu, voice and policy types */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_GUEST_MOD_HOOKS_ABI 1u
#define PC_GUEST_MOD_HOOKS_BIND_SYMBOL "skyrim_guest_bind_mod_hooks_v1"

enum {
    PC_GUEST_MOD_HOOK_FUZ_MENU = 1u,
    PC_GUEST_MOD_HOOK_FUZ_WORLD = 2u,
    PC_GUEST_MOD_HOOK_VM_LIFECYCLE = 4u,
    PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE = 8u,
    PC_GUEST_MOD_HOOK_CAMERA_OBSERVE = 16u,
    PC_GUEST_MOD_HOOK_CHARGEN_SLIDER = 32u,
    /* Witnesses fresh writes and publication from the save owner thread. */
    PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE = 64u,
    PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE = 128u,
    /* Selection enables the pinned guest-side mode change. */
    PC_GUEST_MOD_HOOK_INVENTORY_FIT = 256u,
    PC_GUEST_MOD_HOOK_SAFETY_LOAD = 512u,
    PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE = 1024u,
    PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE = 2048u,
    PC_GUEST_MOD_HOOK_FRESH_LOAD = 4096u,
    PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE = 8192u,
    PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE = 16384u,
    PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE = 32768u,
    PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE = 65536u,
    PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE = 131072u
};
#define PC_GUEST_MOD_HOOKS_ORIGINAL (PC_GUEST_MOD_HOOK_FUZ_MENU | PC_GUEST_MOD_HOOK_FUZ_WORLD | \
    PC_GUEST_MOD_HOOK_VM_LIFECYCLE | PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE | PC_GUEST_MOD_HOOK_CHARGEN_SLIDER | \
    PC_GUEST_MOD_HOOK_CAMERA_OBSERVE)
#define PC_GUEST_MOD_HOOKS_OBSERVED (PC_GUEST_MOD_HOOKS_ORIGINAL | PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE | \
    PC_GUEST_MOD_HOOK_INVENTORY_FIT)
#define PC_GUEST_MOD_HOOKS_SAFETY (PC_GUEST_MOD_HOOKS_OBSERVED | PC_GUEST_MOD_HOOK_SAFETY_LOAD)
#define PC_GUEST_MOD_HOOKS_SAVE (PC_GUEST_MOD_HOOKS_SAFETY | PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE)
#define PC_GUEST_MOD_HOOKS_CAMERA_RENDER (PC_GUEST_MOD_HOOKS_SAVE | PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE)
#define PC_GUEST_MOD_HOOKS_ECE_TRI (PC_GUEST_MOD_HOOKS_CAMERA_RENDER | PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE)
#define PC_GUEST_MOD_HOOKS_FRESH_LOAD (PC_GUEST_MOD_HOOKS_ECE_TRI | PC_GUEST_MOD_HOOK_FRESH_LOAD)
#define PC_GUEST_MOD_HOOKS_CAMERA_CULL (PC_GUEST_MOD_HOOKS_FRESH_LOAD | PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE)
#define PC_GUEST_MOD_HOOKS_ECE_NOSE (PC_GUEST_MOD_HOOKS_CAMERA_CULL | PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE)
#define PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX (PC_GUEST_MOD_HOOKS_ECE_NOSE | PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE)
#define PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE (PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX | PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE)
#define PC_GUEST_MOD_HOOKS_DEFINED (PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE | PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE)
#define PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED (PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE | PC_GUEST_MOD_HOOK_VM_LIFECYCLE)

/* Function types come from the reviewed experiment headers. */
typedef struct {
    void (*forget_response)(uint32_t response);
    void (*choose_menu_voice)(const struct CPU *caller, uint32_t argument_slot, uint32_t response,
                              SkyrimFuzLiftChoice *choice);
    void (*note_menu_assignment)(uint32_t response, const SkyrimFuzLiftChoice *choice);
    SkyrimFuzMenuScope (*capture_menu)(uint32_t manager, uint32_t node, uint32_t queued_title);
    bool (*force_menu)(const SkyrimFuzMenuScope *scope, uint32_t return_address, uint32_t queued_title);
} PcGuestModFuzMenuHooks;

typedef struct {
    SkyrimFuzWorldScope (*capture_world)(uint32_t response, uint32_t voice);
    bool (*admit_world)(const SkyrimFuzWorldScope *scope, uint32_t return_address, uint32_t title);
    void (*world_after_copy)(uint32_t array, uint32_t index, uint32_t source, const SkyrimFuzWorldScope *scope);
    void (*release_chatter_title)(uint32_t title);
    bool (*world_general)(uint32_t array, uint32_t record);
} PcGuestModFuzWorldHooks;

/* MOD_FIRST_WAVE_PLAN.md section 4.4: before the original revert, after the
 * original VM load. form_deleted runs after the original sleep-unregister
 * call in f_8D6550 (0x8D657A) returns, with the VM and the deleted handle
 * read before that call (plan P3-4). */
typedef struct {
    void (*before_revert)(unsigned site);
    void (*after_load)(int succeeded);
    void (*form_deleted)(struct CPU *cpu, uint32_t vm, uint64_t handle);
} PcGuestModVmHooks;

typedef struct {
    bool (*skip_open_warm)(void);
} PcGuestModChargenHooks;

/* The game invokes these on the owner of the live character menu. */
typedef struct {
    bool (*pending)(void);
    bool (*nose_height)(uint32_t menu, uint32_t actor, uint32_t base,
                        float minimum, float maximum, float *value);
    void (*did_apply)(void);
} PcGuestModChargenSliderHooks;

/* Runs after the original Classic camera transition on its calling thread. */
typedef struct {
    void (*observe_return)(uint32_t receiver);
} PcGuestModCameraHooks;

/* Runs at the two reviewed calls immediately before the original dispatch. */
typedef struct {
    void (*observe)(uint32_t site, uint32_t dispatcher, uint32_t event);
} PcGuestModLoadingMenuHooks;

typedef struct {
    bool (*bypass_reclamation)(void);
} PcGuestModSafetyLoadHooks;

/* Runs synchronously before the original culling routine consumes the frame's lists. */
typedef struct {
    void (*observe)(uint32_t frame, uint32_t first, uint32_t second);
} PcGuestModCameraRenderHooks;

/* A synchronous, exact-path observation around the original FaceGen TRI parser. */
typedef struct {
    uint64_t (*begin)(const struct CPU *cpu, uint32_t path, uint32_t stream);
    void (*finish)(const struct CPU *cpu, uint64_t ticket, uint8_t parsed);
} PcGuestModEceTriHooks;

typedef struct {
    uint64_t (*begin)(const struct CPU *, uint32_t stack, uint32_t temporary_path);
    uint64_t (*current)(const struct CPU *, unsigned phase);
    void (*event)(const struct CPU *, uint64_t attempt, unsigned site, int succeeded);
} PcGuestModSaveHooks;

/* Arguments are original spilled guest values. Only Begin, ReadBegin and
 * VmBegin return an owner token. Until selected all events are passive. */
enum SkyrimFreshLoadNotice {
    SkyrimFreshBegin = 1, SkyrimFreshCached, SkyrimFreshOpenEnter, SkyrimFreshOpenLeave,
    SkyrimFreshSlotEnter, SkyrimFreshSlotLeave, SkyrimFreshReadBegin, SkyrimFreshReadLeave,
    SkyrimFreshVmBegin, SkyrimFreshVmLeave, SkyrimFreshFinish, SkyrimFreshPluginsReady
};
typedef struct {
    uint64_t (*notify)(const struct CPU *, unsigned site, uint64_t ticket, uint32_t first, uint32_t second);
} PcGuestModFreshLoadHooks;

/* The synchronous frame, producer and cull observations never change a game object. */
enum SkyrimCameraCullNotice {
    SkyrimCameraCullPrepare, SkyrimCameraCullPrepared, SkyrimCameraCullFinish,
    SkyrimCameraCullProducer, SkyrimCameraCullDispatch, SkyrimCameraCullDispatched,
    SkyrimCameraCullDecision
};
typedef struct {
    bool (*notify)(enum SkyrimCameraCullNotice, uint32_t first, uint32_t second, unsigned third);
} PcGuestModCameraCullHooks;

/* Synchronous witness of the current stock nose morph. A parse receipt is
 * private to its owning CPU and never leaves this interface. */
enum SkyrimEceNoseNotice {
    SkyrimEceNoseMenu = 1, SkyrimEceNoseMenuEnd, SkyrimEceNoseBegin, SkyrimEceNoseEnd,
    SkyrimEceNoseFace, SkyrimEceNoseHead, SkyrimEceNoseHeadEnd, SkyrimEceNoseLookup,
    SkyrimEceNoseLookupEnd, SkyrimEceNoseParse, SkyrimEceNoseParsed, SkyrimEceNoseConstructed,
    SkyrimEceNoseRetiring, SkyrimEceNoseMorph, SkyrimEceNoseMorphRecord, SkyrimEceNosePartLayout, SkyrimEceNoseParseDone
};
typedef struct {
    uint64_t (*notify)(const struct CPU *, enum SkyrimEceNoseNotice, uint32_t first, uint32_t second,
                       uint32_t third, uint32_t fourth, uint64_t receipt);
} PcGuestModEceNoseHooks;

/* Runs around the original requested script's parse and link on its owner. */
typedef struct {
    bool (*begin)(const struct CPU *, uint32_t name, uint32_t loader);
    uint64_t (*end)(const struct CPU *, bool parsed);
    void (*linked)(const struct CPU *, uint64_t epoch, bool linked);
} PcGuestModHighHeelPexHooks;

enum SkyrimHighHeelSceneNotice {
    SkyrimHighHeelSceneArm, SkyrimHighHeelSceneRoot, SkyrimHighHeelSceneEnter,
    SkyrimHighHeelSceneLeave, SkyrimHighHeelSceneMember, SkyrimHighHeelSceneFinish
};
enum { SkyrimHighHeelSceneReady = 1, SkyrimHighHeelSceneActive };
/* Member's third argument is zero for names-only or this value after world-transform writes. */
enum { SkyrimHighHeelScenePose = 1 };
typedef struct {
    bool (*notify)(const struct CPU *, enum SkyrimHighHeelSceneNotice, uint32_t first, uint32_t second, uint32_t third);
} PcGuestModHighHeelSceneHooks;

/* Startup supplies the finalized catalogue once. The producer then lends
 * the effect and its retained target only during the synchronous callback. */
typedef struct {
    void (*plugins_ready)(const struct CPU *, uint32_t handler);
    void (*phase)(const struct CPU *, uint32_t effect, uint32_t arguments, uint32_t site);
} PcGuestModHighHeelEffectHooks;

/* Host implementations. groups names the selection; only the tables of the
 * selected groups are read, and each of their entries must be set. */
typedef struct {
    uint32_t version, bytes;
    uint32_t groups, reserved;
    PcGuestModFuzMenuHooks fuz_menu;
    PcGuestModFuzWorldHooks fuz_world;
    PcGuestModVmHooks vm_lifecycle;
    PcGuestModChargenHooks chargen_precache;
    PcGuestModChargenSliderHooks chargen_slider;
    PcGuestModCameraHooks camera;
    PcGuestModLoadingMenuHooks loading_menu;
    PcGuestModSafetyLoadHooks safety_load;
    PcGuestModSaveHooks save;
    PcGuestModCameraRenderHooks camera_render;
    PcGuestModEceTriHooks ece_tri;
    PcGuestModFreshLoadHooks fresh_load;
    PcGuestModCameraCullHooks camera_cull;
    PcGuestModEceNoseHooks ece_nose;
    PcGuestModHighHeelPexHooks highheel_pex;
    PcGuestModHighHeelSceneHooks highheel_scene;
    PcGuestModHighHeelEffectHooks highheel_effect;
} PcGuestModHooks;

/* Hosts and guests built with the original v1 table still use this prefix. */
#define PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES offsetof(PcGuestModHooks, loading_menu)
#define PC_GUEST_MOD_HOOKS_OBSERVED_BYTES offsetof(PcGuestModHooks, safety_load)
#define PC_GUEST_MOD_HOOKS_SAFETY_BYTES offsetof(PcGuestModHooks, save)
#define PC_GUEST_MOD_HOOKS_SAVE_BYTES offsetof(PcGuestModHooks, camera_render)
#define PC_GUEST_MOD_HOOKS_CAMERA_RENDER_BYTES offsetof(PcGuestModHooks, ece_tri)
#define PC_GUEST_MOD_HOOKS_ECE_TRI_BYTES offsetof(PcGuestModHooks, fresh_load)
#define PC_GUEST_MOD_HOOKS_FRESH_LOAD_BYTES offsetof(PcGuestModHooks, camera_cull)
#define PC_GUEST_MOD_HOOKS_CAMERA_CULL_BYTES offsetof(PcGuestModHooks, ece_nose)
#define PC_GUEST_MOD_HOOKS_ECE_NOSE_BYTES offsetof(PcGuestModHooks, highheel_pex)
#define PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX_BYTES offsetof(PcGuestModHooks, highheel_scene)
#define PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE_BYTES offsetof(PcGuestModHooks, highheel_effect)

/* Guest reply. select copies the selected tables and succeeds at most once. */
typedef struct {
    uint32_t version, bytes;
    uint32_t compiled_groups, table_bytes;
    int (*select)(const PcGuestModHooks *implementations);
} PcGuestModHooksGuest;

typedef const PcGuestModHooksGuest *(*PcGuestBindModHooks)(uint32_t version, uint32_t table_bytes);

static inline int pc_guest_mod_hooks_groups_valid(uint32_t groups) {
    return (groups & ~(uint32_t)PC_GUEST_MOD_HOOKS_DEFINED) == 0
        /* The world stage reuses the menu's response assignment. */
        && (!(groups & PC_GUEST_MOD_HOOK_FUZ_WORLD) || (groups & PC_GUEST_MOD_HOOK_FUZ_MENU))
        && (!(groups & PC_GUEST_MOD_HOOK_SAFETY_LOAD) || (groups & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE))
        && (!(groups & PC_GUEST_MOD_HOOK_FRESH_LOAD) ||
            (groups & PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED) == PC_GUEST_MOD_HOOK_FRESH_LOAD_REQUIRED)
        && (!(groups & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE) || (groups & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE))
        && (!(groups & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE) || (groups & PC_GUEST_MOD_HOOK_VM_LIFECYCLE))
        && (!(groups & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE) || (groups & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE));
}

static inline uint32_t pc_guest_mod_hooks_groups_for_bytes(uint32_t bytes) {
    if (bytes == sizeof(PcGuestModHooks)) return PC_GUEST_MOD_HOOKS_DEFINED;
    if (bytes == PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE_BYTES) return PC_GUEST_MOD_HOOKS_HIGHHEEL_SCENE;
    if (bytes == PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX_BYTES) return PC_GUEST_MOD_HOOKS_HIGHHEEL_PEX;
    if (bytes == PC_GUEST_MOD_HOOKS_ECE_NOSE_BYTES) return PC_GUEST_MOD_HOOKS_ECE_NOSE;
    if (bytes == PC_GUEST_MOD_HOOKS_CAMERA_CULL_BYTES) return PC_GUEST_MOD_HOOKS_CAMERA_CULL;
    if (bytes == PC_GUEST_MOD_HOOKS_FRESH_LOAD_BYTES) return PC_GUEST_MOD_HOOKS_FRESH_LOAD;
    if (bytes == PC_GUEST_MOD_HOOKS_ECE_TRI_BYTES) return PC_GUEST_MOD_HOOKS_ECE_TRI;
    if (bytes == PC_GUEST_MOD_HOOKS_CAMERA_RENDER_BYTES) return PC_GUEST_MOD_HOOKS_CAMERA_RENDER;
    if (bytes == PC_GUEST_MOD_HOOKS_SAVE_BYTES) return PC_GUEST_MOD_HOOKS_SAVE;
    if (bytes == PC_GUEST_MOD_HOOKS_SAFETY_BYTES) return PC_GUEST_MOD_HOOKS_SAFETY;
    if (bytes == PC_GUEST_MOD_HOOKS_OBSERVED_BYTES) return PC_GUEST_MOD_HOOKS_OBSERVED;
    if (bytes == PC_GUEST_MOD_HOOKS_ORIGINAL_BYTES) return PC_GUEST_MOD_HOOKS_ORIGINAL;
    return 0;
}

static inline int pc_guest_mod_hooks_table_valid(const PcGuestModHooks *t) {
    if (!t || t->version != PC_GUEST_MOD_HOOKS_ABI || t->reserved || !pc_guest_mod_hooks_groups_valid(t->groups))
        return 0;
    const uint32_t supported = pc_guest_mod_hooks_groups_for_bytes(t->bytes);
    const uint32_t g = t->groups;
    if (!supported || (g & ~supported)) return 0;
    if ((g & PC_GUEST_MOD_HOOK_FUZ_MENU) && (!t->fuz_menu.forget_response || !t->fuz_menu.choose_menu_voice
            || !t->fuz_menu.note_menu_assignment || !t->fuz_menu.capture_menu || !t->fuz_menu.force_menu))
        return 0;
    if ((g & PC_GUEST_MOD_HOOK_FUZ_WORLD) && (!t->fuz_world.capture_world || !t->fuz_world.admit_world
            || !t->fuz_world.world_after_copy || !t->fuz_world.release_chatter_title
            || !t->fuz_world.world_general))
        return 0;
    if ((g & PC_GUEST_MOD_HOOK_VM_LIFECYCLE) && (!t->vm_lifecycle.before_revert || !t->vm_lifecycle.after_load
            || !t->vm_lifecycle.form_deleted))
        return 0;
    if ((g & PC_GUEST_MOD_HOOK_CHARGEN_PRECACHE) && !t->chargen_precache.skip_open_warm) return 0;
    if ((g & PC_GUEST_MOD_HOOK_CHARGEN_SLIDER) &&
        (!t->chargen_slider.pending || !t->chargen_slider.nose_height || !t->chargen_slider.did_apply)) return 0;
    if ((g & PC_GUEST_MOD_HOOK_CAMERA_OBSERVE) && !t->camera.observe_return) return 0;
    if ((g & PC_GUEST_MOD_HOOK_LOADING_MENU_OBSERVE) && !t->loading_menu.observe) return 0;
    if ((g & PC_GUEST_MOD_HOOK_SAFETY_LOAD) && !t->safety_load.bypass_reclamation) return 0;
    if ((g & PC_GUEST_MOD_HOOK_SAVE_LIFECYCLE) && (!t->save.begin || !t->save.current || !t->save.event)) return 0;
    if ((g & PC_GUEST_MOD_HOOK_CAMERA_RENDER_OBSERVE) && !t->camera_render.observe) return 0;
    if ((g & PC_GUEST_MOD_HOOK_ECE_TRI_OBSERVE) && (!t->ece_tri.begin || !t->ece_tri.finish)) return 0;
    if ((g & PC_GUEST_MOD_HOOK_FRESH_LOAD) && !t->fresh_load.notify) return 0;
    if ((g & PC_GUEST_MOD_HOOK_CAMERA_CULL_OBSERVE) && !t->camera_cull.notify) return 0;
    if ((g & PC_GUEST_MOD_HOOK_ECE_NOSE_OBSERVE) && !t->ece_nose.notify) return 0;
    if ((g & PC_GUEST_MOD_HOOK_HIGHHEEL_PEX_OBSERVE) &&
        (!t->highheel_pex.begin || !t->highheel_pex.end || !t->highheel_pex.linked)) return 0;
    if ((g & PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE) && !t->highheel_scene.notify) return 0;
    if ((g & PC_GUEST_MOD_HOOK_HIGHHEEL_EFFECT_OBSERVE) &&
        (!t->highheel_effect.plugins_ready || !t->highheel_effect.phase)) return 0;
    return 1;
}

/* Guest side (pc_guest_mod_hooks_library.c): names called by staged units in
 * addition to the skyrim_fuz_host_* imports of the Fuz seam headers. */
void skyrim_vm_host_before_revert(unsigned site);
void skyrim_vm_host_after_load(int succeeded);
void skyrim_vm_host_form_deleted(struct CPU *cpu, uint32_t vm, uint64_t handle);
bool skyrim_chargen_probe_skip_open_warm(void);
bool skyrim_racemenu_probe_pending(void);
bool skyrim_racemenu_probe_nose_height(uint32_t menu, uint32_t actor, uint32_t base,
                                     float minimum, float maximum, float *value);
void skyrim_racemenu_probe_did_apply(void);
void skyrim_camera_observe_return(uint32_t receiver);
void skyrim_camera_observe_render(uint32_t frame, uint32_t first, uint32_t second);
bool skyrim_camera_cull_guest_notify(enum SkyrimCameraCullNotice, uint32_t first, uint32_t second, unsigned third);
extern bool skyrim_camera_cull_guest_selected;
extern unsigned skyrim_camera_cull_guest_frames;
uint64_t skyrim_ece_tri_begin(const struct CPU *, uint32_t path, uint32_t stream);
void skyrim_ece_tri_finish(const struct CPU *, uint64_t ticket, uint8_t parsed);
extern bool skyrim_ece_nose_guest_selected;
uint64_t skyrim_ece_nose_guest_notify(const struct CPU *, enum SkyrimEceNoseNotice, uint32_t first,
                                     uint32_t second, uint32_t third, uint32_t fourth, uint64_t receipt);
extern bool skyrim_highheel_pex_guest_selected;
bool skyrim_highheel_pex_begin(const struct CPU *, uint32_t name, uint32_t loader);
uint64_t skyrim_highheel_pex_end(const struct CPU *, bool parsed);
void skyrim_highheel_pex_linked(const struct CPU *, uint64_t epoch, bool linked);
extern unsigned skyrim_highheel_scene_guest_state;
bool skyrim_highheel_scene_guest_notify(const struct CPU *, enum SkyrimHighHeelSceneNotice,
                                         uint32_t first, uint32_t second, uint32_t third);
extern bool skyrim_highheel_effect_guest_selected;
void skyrim_highheel_effect_plugins_ready(const struct CPU *, uint32_t handler);
void skyrim_highheel_effect_phase(const struct CPU *, uint32_t effect, uint32_t arguments, uint32_t site);
void skyrim_loading_menu_observe(uint32_t site, uint32_t dispatcher, uint32_t event);
bool skyrim_inventory_fit_enabled(void);
bool skyrim_safety_load_bypass_reclamation(void);
uint64_t skyrim_save_notify_begin(const struct CPU *cpu, uint32_t stack, uint32_t temporary_path);
uint64_t skyrim_save_notify_current(const struct CPU *cpu, unsigned phase);
void skyrim_save_notify_event(const struct CPU *cpu, uint64_t attempt, unsigned site, int succeeded);
uint64_t skyrim_load_notify(const struct CPU *, unsigned site, uint64_t ticket, uint32_t first, uint32_t second);

/* Host side (pc_guest_mod_hooks_host.c). */
int pc_guest_bind_mod_hooks(void *library);  /* from pc_guest_open; a missing symbol is not an error */
void pc_guest_forget_mod_hooks(void);         /* from pc_guest_open when it rejects the library later */
uint32_t pc_guest_mod_hooks_compiled(void);   /* groups the opened guest carries, 0 without the symbol */
/* Call once on the startup thread after pc_guest_open and before the guest
 * starts. groups must be compiled into the guest; implementations may be NULL
 * only when groups is 0. A failed call changes nothing and may be repeated. */
int pc_guest_select_mod_hooks(uint32_t groups, const PcGuestModHooks *implementations);
uint32_t pc_guest_mod_hooks_selected(void);

#ifdef __cplusplus
}
#endif
