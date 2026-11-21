/* SPDX-License-Identifier: GPL-3.0-only
 * Classic 1.7.3 control events (plan P3-4), included by the staged
 * skse_system.c in MOD hook builds. RegisterForControl, UnregisterForControl
 * and UnregisterForAllControls for Form, Alias and ActiveMagicEffect use the
 * reviewed experiments/classic-control-events bridge. The staged skse_input.c
 * reports each mapped edge immediately before its GFX call; OnControlDown
 * (string) and OnControlUp (string, float) are queued through send_event.
 *
 * Subscriptions belong to one VM generation: on the first use in a new one
 * they are dropped without the old handle policy, as Classic does after its
 * revert. Form deletion removes a handle's rows after the original
 * sleep-unregister call returns (guest VM_LIFECYCLE hook).
 *
 * Action names are the game's interned BSFixedString data pointers. A
 * subscription keeps its own string-cache reference (0xA51210 sets it,
 * 0xA511B0 releases it, as send_event does for event names); the set must
 * return the same interned pointer or the registration is refused.
 */
#ifdef SKYRIM_IOS_MOD_HOOKS
#include "classic_vm_epoch.h"
#include "control_bridge.h"

enum { CLASSIC_CONTROL_ACTIONS = 64 };

static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER;
static ClassicControlBridge control_bridge;
static uint64_t control_epoch;
static uint32_t control_registry;
static CPU *control_cpu;  /* the CPU of the call holding control_lock */
static _Thread_local bool control_active;
static struct { uint32_t action, holder; unsigned refs; } control_actions[CLASSIC_CONTROL_ACTIONS];
static bool control_refused;

static void control_handle_ref(void *context, uint64_t handle, bool retain) {
    (void)context;
    handle_ref(control_cpu, handle, retain);
}

static void control_action_ref(void *context, uint32_t action, bool retain) {
    (void)context;
    for (unsigned i = 0; i < CLASSIC_CONTROL_ACTIONS; ++i) {
        if (control_actions[i].action != action || !control_actions[i].refs) continue;
        if (retain) { ++control_actions[i].refs; return; }
        if (--control_actions[i].refs) return;
        skse_member(control_cpu, 0xA511B0, control_actions[i].holder, 0, NULL);
        gfree(control_actions[i].holder);
        memset(&control_actions[i], 0, sizeof control_actions[i]);
        return;
    }
    if (!retain) return;
    for (unsigned i = 0; i < CLASSIC_CONTROL_ACTIONS; ++i) {
        if (control_actions[i].refs) continue;
        const uint32_t holder = gmalloc(4), text = gstrdup(str(action));
        M32(holder) = 0;
        skse_member(control_cpu, 0xA51210, holder, 1, &text);
        gfree(text);
        if (M32(holder) != action) {
            skse_member(control_cpu, 0xA511B0, holder, 0, NULL);
            gfree(holder);
            break;
        }
        control_actions[i].action = action;
        control_actions[i].holder = holder;
        control_actions[i].refs = 1;
        return;
    }
    control_refused = true;  /* table full or not the interned pointer */
}

static void control_enqueue(void *context, const ClassicControlEvent *event) {
    (void)context;
    const uint32_t values = gmalloc(16);
    memset(GPTR(values), 0, 16);
    vm_string(control_cpu, values, str(event->action));
    if (event->argc > 1) { M32(values + 8) = 4; M32(values + 12) = float_bits(event->held_seconds); }
    const Registration recipient = {.handle = event->handle, .registry = control_registry};
    send_event(control_cpu, &recipient, event->name, event->argc, values);
    vm_clear(control_cpu, values);
    gfree(values);
}

/* With control_lock held: the bridge for the current generation, or false. */
static bool control_enter(CPU *c) {
    SkyrimVmScope scope;
    if (control_active || !skyrim_vm_scope_current(&scope)) return false;
    control_active = true;
    pthread_mutex_lock(&control_lock);
    control_cpu = c;
    if (!control_bridge.vm.enqueue)
        control_bridge.vm = (ClassicControlVm){NULL, control_handle_ref, control_action_ref, control_enqueue};
    if (control_epoch != scope.epoch) {
        classic_control_after_vm_revert(&control_bridge);  /* the old VM's handle policy is not used */
        control_epoch = scope.epoch;
        control_registry = scope.registry;
    }
    return true;
}

static void control_leave(void) {
    control_cpu = NULL;
    pthread_mutex_unlock(&control_lock);
    control_active = false;
}

static int skse_classic_control(CPU *c, unsigned op, const uint32_t *args, uint32_t registry, uint32_t base) {
    if (!control_enter(c)) return 0;
    int ok = 0;
    if (registry == control_registry) {
        const uint64_t handle = base_handle(base);
        const ClassicControlOperation operation = op == S_REG_CONTROL ? CLASSIC_REGISTER_CONTROL
            : op == S_UNREG_CONTROL ? CLASSIC_UNREGISTER_CONTROL : CLASSIC_UNREGISTER_ALL_CONTROLS;
        control_refused = false;
        ok = classic_control_change(&control_bridge, operation, handle,
                                    operation == CLASSIC_UNREGISTER_ALL_CONTROLS ? 0 : args[0]);
        if (control_refused) {
            /* The action reference was not taken: undo the row so counts stay balanced. */
            classic_control_change(&control_bridge, CLASSIC_UNREGISTER_CONTROL, handle, args[0]);
            ok = 0;
        }
    }
    control_leave();
    return ok;
}

/* Called by the staged skse_input.c before each mapped GFX edge. */
void skyrim_classic_control_edge(CPU *c, uint32_t action, int down, float held_seconds) {
    if (!action || !control_enter(c)) return;
    classic_control_edge(&control_bridge, action, down != 0, held_seconds);
    control_leave();
}

/* Guest VM_LIFECYCLE hook, after the original sleep-unregister returned. */
void skyrim_classic_control_form_deleted(CPU *c, uint32_t vm, uint64_t handle) {
    if (!control_enter(c)) return;
    if (vm == control_registry) classic_control_unregister_all(&control_bridge, handle);
    control_leave();
}
#endif
