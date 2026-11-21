/* classic_skse_mod_event.h with the definitions skse_system.c gives it. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct CPU { uint32_t r[8]; } CPU;
static uint8_t guest[0x10000];
#define M32(a) (*(uint32_t *)(void *)(guest + (uint32_t)(a)))
#define LOG(...) ((void)0)
enum { S_MODEVENT_CREATE = 1330, S_MODEVENT_PUSH_BOOL, S_MODEVENT_SEND, S_MODEVENT_RELEASE };

typedef struct Registration {
    struct Registration *next;
    uint64_t handle;
    uint32_t registry;
    unsigned kind, key, active;
    char *name, *callback;
    uint32_t down_tick;
} Registration;
static Registration *registrations;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static int refs;
static uint32_t heap = 0x8000;
static const char *str(uint32_t p) { return (const char *)guest + p; }
static uint32_t gmalloc(unsigned bytes) { const uint32_t a = heap; heap += (bytes + 7) & ~7u; return a; }
static void gfree(uint32_t address) { (void)address; }
static void handle_ref(CPU *c, uint64_t handle, int add) { (void)c; (void)handle; refs += add ? 1 : -1; }

static struct { uint64_t handle; char callback[32]; unsigned count; uint32_t types[4], bits[4]; } sent[8];
static unsigned sent_count;
static bool reenter;
static int skse_classic_mod_event(CPU *, unsigned, const uint32_t *, uint32_t, uint32_t);
static void send_event(CPU *c, const Registration *r, const char *event, unsigned count, uint32_t values) {
    assert(sent_count < 8 && count <= 4);
    sent[sent_count].handle = r->handle;
    strcpy(sent[sent_count].callback, event);
    sent[sent_count].count = count;
    for (unsigned i = 0; i < count; ++i) {
        sent[sent_count].types[i] = M32(values + 8 * i);
        sent[sent_count].bits[i] = M32(values + 8 * i + 4);
    }
    ++sent_count;
    if (reenter) {  /* a nested ModEvent call on the same thread is refused */
        const uint32_t args[2] = {1, 1};
        assert(!skse_classic_mod_event(c, S_MODEVENT_PUSH_BOOL, args, 0x500, 0x600));
    }
}

#include "classic_skse_mod_event.h"

static Registration rows[5];
static void subscribe(void) {
    static char oil[] = "Frostfall_ForceOilMeterDisplay", other[] = "OtherEvent";
    static char force[] = "ForceMeterDisplay", remove_cb[] = "RemoveMeter";
    rows[0] = (Registration){&rows[1], 0x30, 0x500, 2, 0, 0, oil, force, 0};
    rows[1] = (Registration){&rows[2], 0x10, 0x500, 2, 0, 0, oil, force, 0};
    rows[2] = (Registration){&rows[3], 0x20, 0x777, 2, 0, 0, oil, force, 0};   /* another VM */
    rows[3] = (Registration){&rows[4], 0x40, 0x500, 1, 0, 0, oil, force, 0};   /* a menu row */
    rows[4] = (Registration){NULL, 0x50, 0x500, 2, 0, 0, other, remove_cb, 0};
    registrations = rows;
}

static int call(unsigned op, uint32_t a, uint32_t b, uint32_t *out) {
    CPU c = {{0}};
    c.r[4] = 0x100;
    M32(0x100 + 12) = 9;  /* stack id */
    const uint32_t args[2] = {a, b};
    M32(0x600 + 4) = 0xdeadbeef;
    const int ok = skse_classic_mod_event(&c, op, args, 0x500, 0x600);
    if (out) *out = M32(0x600 + 4);
    return ok;
}

int main(void) {
    strcpy((char *)guest + 0x200, "Frostfall_ForceOilMeterDisplay");
    subscribe();
    uint32_t h;
    /* Before the VM is open, nothing runs. */
    assert(!call(S_MODEVENT_CREATE, 0x200, 0, &h));
    skyrim_vm_epoch_registered(0x500);

    /* Create, PushBool, Send: two recipients of this VM, ascending handles, one bool. */
    assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == 1);
    assert(call(S_MODEVENT_PUSH_BOOL, h, 0x101, NULL));  /* only the low byte is the bool */
    uint32_t delivered;
    reenter = true;
    assert(call(S_MODEVENT_SEND, h, 0, &delivered) && delivered == 1);
    reenter = false;
    assert(sent_count == 2 && sent[0].handle == 0x10 && sent[1].handle == 0x30);
    assert(!strcmp(sent[0].callback, "ForceMeterDisplay") && sent[0].count == 1);
    assert(sent[0].types[0] == 5 && sent[0].bits[0] == 1 && refs == 0);
    /* The handle was consumed: a second send and a push fail (result false). */
    assert(call(S_MODEVENT_SEND, h, 0, &delivered) && !delivered);

    /* Zero arguments, Release without sending, and slot reuse. */
    assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == 1);
    assert(call(S_MODEVENT_RELEASE, h, 0, NULL));
    assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == 1);
    sent_count = 0;
    assert(call(S_MODEVENT_SEND, h, 0, &delivered) && delivered == 1 && sent_count == 2 && !sent[0].count);

    /* Another registry is refused outright. */
    CPU c = {{0}};
    const uint32_t args[2] = {0x200, 0};
    assert(!skse_classic_mod_event(&c, S_MODEVENT_CREATE, args, 0x777, 0x600));

    /* A handle from before a load is rejected in the next generation, even in the same slot. */
    assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == 1);
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    assert(!call(S_MODEVENT_SEND, h, 0, &delivered));  /* closed while reverting */
    skyrim_vm_epoch_after_load(1);
    sent_count = 0;
    assert(call(S_MODEVENT_SEND, h, 0, &delivered) && !delivered && !sent_count);
    assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == 1);

    /* Capacity: 32 slots, then 0. */
    for (int n = 2; n <= CLASSIC_MOD_EVENT_SLOTS; ++n) assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == (uint32_t)n);
    assert(call(S_MODEVENT_CREATE, 0x200, 0, &h) && h == 0);
    assert(refs == 0);
    return 0;
}
