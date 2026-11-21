/* SPDX-License-Identifier: GPL-3.0-only
 * Classic 1.7.3 ModEvent statics with boolean packets (plan P3-3), included by
 * the staged skse_system.c in MOD hook builds. The packet storage is
 * experiments/classic-mod-events/mod_event.c; delivery uses this file's
 * registration list and send_event. Only Create, PushBool, Send and Release
 * are registered; other Push types are reported as missing SKSE functions.
 *
 * The packet table belongs to one VM generation (classic_vm_epoch.h): a
 * handle from before a load or reset is rejected. Registrations are not
 * cleared on revert; that stays as before (MOD_FIRST_WAVE_PLAN P1-2) until
 * the co-save restore exists. Packets created and never sent or released
 * keep their slot until the next generation; Create returns 0 when all 32
 * slots are taken.
 */
#ifdef SKYRIM_IOS_MOD_HOOKS
#include "classic_vm_epoch.h"
#include "mod_event.h"

enum { CLASSIC_MOD_EVENT_TARGETS = 64, CLASSIC_PAPYRUS_BOOL = 5 };

static pthread_mutex_t mod_event_lock = PTHREAD_MUTEX_INITIALIZER;
static ClassicModEvents mod_events;
static _Thread_local bool mod_event_active;

typedef struct {
    CPU *cpu;
    uint32_t registry;
    unsigned delivered;
    bool overflow;
} ModEventDelivery;

typedef struct {
    Registration recipient;
    char *callback;
} ModEventTarget;

static int mod_event_by_handle(const void *a, const void *b) {
    const ModEventTarget *left = a, *right = b;
    return (left->recipient.handle > right->recipient.handle) - (left->recipient.handle < right->recipient.handle);
}

/* Borrowed packet: copy what is needed before any guest call. */
static void mod_event_deliver(void *context, const ClassicModEventPacket *packet) {
    ModEventDelivery *delivery = context;
    ModEventTarget targets[CLASSIC_MOD_EVENT_TARGETS];
    size_t count = 0;
    pthread_mutex_lock(&event_lock);
    for (Registration *r = registrations; r; r = r->next) {
        if (r->kind != 2 || r->registry != delivery->registry || strcmp(r->name, packet->name)) continue;
        if (count == CLASSIC_MOD_EVENT_TARGETS) { delivery->overflow = true; break; }
        char *callback = strdup(r->callback);
        if (!callback) { delivery->overflow = true; break; }
        targets[count++] = (ModEventTarget){.recipient = {.handle = r->handle, .registry = r->registry},
                                            .callback = callback};
        handle_ref(delivery->cpu, r->handle, 1);
    }
    pthread_mutex_unlock(&event_lock);
    qsort(targets, count, sizeof targets[0], mod_event_by_handle);  /* Classic visits ascending handles */
    const uint32_t values = packet->argc ? gmalloc(8u * packet->argc) : 0;
    if (packet->argc && !values) delivery->overflow = true;
    for (unsigned i = 0; values && i < packet->argc; ++i) {
        M32(values + 8 * i) = CLASSIC_PAPYRUS_BOOL;
        M32(values + 8 * i + 4) = packet->args[i];
    }
    for (size_t i = 0; i < count; ++i) {
        if (!packet->argc || values) {
            send_event(delivery->cpu, &targets[i].recipient, targets[i].callback, packet->argc, values);
            ++delivery->delivered;
        }
        handle_ref(delivery->cpu, targets[i].recipient.handle, 0);
        free(targets[i].callback);
    }
    if (values) gfree(values);
}

static int skse_classic_mod_event(CPU *c, unsigned op, const uint32_t *args, uint32_t registry, uint32_t result) {
    SkyrimVmScope scope;
    if (mod_event_active || !skyrim_vm_scope_current(&scope) || scope.registry != registry) return 0;
    mod_event_active = true;
    pthread_mutex_lock(&mod_event_lock);
    if (mod_events.epoch != scope.epoch) classic_mod_events_reset(&mod_events, scope.epoch);
    const int32_t handle = (int32_t)args[0];
    int ok = 1;
    if (op == S_MODEVENT_CREATE) {
        const char *name = str(args[0]);
        const size_t length = strnlen(name, CLASSIC_MOD_EVENT_NAME_MAX + 1);
        /* Native Run frame: return, base, registry, stack id. */
        const uint32_t stack = M32(c->r[4] + 12);
        M32(result + 4) = (uint32_t)classic_mod_event_create(&mod_events, scope.epoch, stack, name, length);
    } else if (op == S_MODEVENT_PUSH_BOOL) {
        classic_mod_event_push_bool(&mod_events, scope.epoch, handle, (uint8_t)args[1] != 0);
    } else if (op == S_MODEVENT_SEND) {
        ModEventDelivery delivery = {.cpu = c, .registry = registry};
        M32(result + 4) = classic_mod_event_send(&mod_events, scope.epoch, handle, mod_event_deliver, &delivery);
        if (delivery.overflow) LOG("SKSE ModEvent: delivery truncated");
    } else if (op == S_MODEVENT_RELEASE) {
        classic_mod_event_release(&mod_events, scope.epoch, handle);
    } else {
        ok = 0;
    }
    pthread_mutex_unlock(&mod_event_lock);
    mod_event_active = false;
    return ok;
}
#endif
