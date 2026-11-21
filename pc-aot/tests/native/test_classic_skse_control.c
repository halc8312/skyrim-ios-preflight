/* classic_skse_control.h with the definitions skse_system.c gives it. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct CPU { uint32_t r[8]; } CPU;
static uint8_t guest[0x20000];
#define M32(a) (*(uint32_t *)(void *)(guest + (uint32_t)(a)))
#define GPTR(a) (guest + (uint32_t)(a))
enum { S_REG_CONTROL = 1340, S_UNREG_CONTROL, S_UNREG_CONTROLS };
typedef struct Registration {
    struct Registration *next;
    uint64_t handle;
    uint32_t registry;
    unsigned kind, key, active;
    char *name, *callback;
    uint32_t down_tick;
} Registration;

enum { ACTIVATE = 0x1000, SPRINT = 0x1100, REGISTRY = 0x500 };
static uint32_t heap = 0x8000;
static int handle_refs, string_refs[2], live_values;
static bool wrong_intern;
static const char *str(uint32_t p) { return (const char *)guest + p; }
static uint32_t gmalloc(unsigned bytes) { const uint32_t a = heap; heap += (bytes + 7) & ~7u; return a; }
static void gfree(uint32_t address) { (void)address; }
static uint32_t gstrdup(const char *text) { const uint32_t a = gmalloc((unsigned)strlen(text) + 1); strcpy((char *)GPTR(a), text); return a; }
static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void handle_ref(CPU *c, uint64_t handle, int add) { assert(c && handle); handle_refs += add ? 1 : -1; }
static int string_index(uint32_t p) { return p == ACTIVATE ? 0 : p == SPRINT ? 1 : -1; }
/* The game's string cache: 0xA51210 sets a holder from text, 0xA511B0 releases it. */
static uint32_t skse_member(CPU *c, uint32_t function, uint32_t self, int count, const uint32_t *args) {
    assert(c);
    if (function == 0xA51210) {
        assert(count == 1);
        const char *text = str(args[0]);
        const uint32_t interned = !strcmp(text, "Activate") ? ACTIVATE : !strcmp(text, "Sprint") ? SPRINT : 0;
        M32(self) = wrong_intern ? interned + 4 : interned;
        if (!wrong_intern && interned) ++string_refs[string_index(interned)];
        return self;
    }
    assert(function == 0xA511B0 && !count);
    const int i = string_index(M32(self));
    if (i >= 0) --string_refs[i];
    M32(self) = 0;
    return 0;
}
static void vm_string(CPU *c, uint32_t value, const char *text) { (void)c; M32(value) = 2; M32(value + 4) = gstrdup(text); ++live_values; }
static void vm_clear(CPU *c, uint32_t value) { (void)c; if (M32(value) == 2) --live_values; M32(value) = 0; }
static uint64_t base_handle(uint32_t base) { return base ? 0x100000000ull + base : 0; }

static struct { uint64_t handle; uint32_t registry; char name[16], action[16]; unsigned count; uint32_t held; } sent[16];
static unsigned sent_count;
static void send_event(CPU *c, const Registration *r, const char *event, unsigned count, uint32_t values) {
    (void)c;
    assert(sent_count < 16 && M32(values) == 2);
    sent[sent_count].handle = r->handle;
    sent[sent_count].registry = r->registry;
    strcpy(sent[sent_count].name, event);
    strcpy(sent[sent_count].action, str(M32(values + 4)));
    sent[sent_count].count = count;
    sent[sent_count].held = count > 1 ? M32(values + 12) : 0;
    assert(count == 1 || M32(values + 8) == 4);
    ++sent_count;
}

#include "classic_skse_control.h"

static CPU cpu;
static int op(unsigned code, uint32_t base, uint32_t action) {
    const uint32_t args[1] = {action};
    return skse_classic_control(&cpu, code, args, REGISTRY, base);
}

int main(void) {
    strcpy((char *)GPTR(ACTIVATE), "Activate");
    strcpy((char *)GPTR(SPRINT), "Sprint");
    assert(!op(S_REG_CONTROL, 0x30, ACTIVATE));  /* VM not open */
    skyrim_vm_epoch_registered(REGISTRY);

    assert(op(S_REG_CONTROL, 0x30, ACTIVATE) && op(S_REG_CONTROL, 0x10, ACTIVATE) && op(S_REG_CONTROL, 0x10, ACTIVATE));
    assert(op(S_REG_CONTROL, 0x10, SPRINT));
    assert(handle_refs == 3 && string_refs[0] == 1 && string_refs[1] == 1);

    skyrim_classic_control_edge(&cpu, ACTIVATE, 1, 0.0f);
    assert(sent_count == 2 && sent[0].handle == 0x100000010ull && sent[1].handle == 0x100000030ull);
    assert(!strcmp(sent[0].name, "OnControlDown") && !strcmp(sent[0].action, "Activate") && sent[0].count == 1);
    assert(sent[0].registry == REGISTRY);
    sent_count = 0;
    skyrim_classic_control_edge(&cpu, ACTIVATE, 0, 1.5f);
    assert(sent_count == 2 && !strcmp(sent[1].name, "OnControlUp") && sent[1].count == 2);
    assert(sent[1].held == float_bits(1.5f) && live_values == 0);

    /* Unregister one, then all for a handle. */
    assert(op(S_UNREG_CONTROL, 0x30, ACTIVATE) && handle_refs == 2);
    sent_count = 0;
    skyrim_classic_control_edge(&cpu, ACTIVATE, 1, 0.0f);
    assert(sent_count == 1 && sent[0].handle == 0x100000010ull);
    assert(op(S_UNREG_CONTROLS, 0x10, 0) && !handle_refs && !string_refs[0] && !string_refs[1]);

    /* A string the cache interns elsewhere is refused with balanced counts. */
    wrong_intern = true;
    assert(!op(S_REG_CONTROL, 0x30, ACTIVATE) && !handle_refs && !string_refs[0]);
    wrong_intern = false;

    /* Another registry is ignored. */
    const uint32_t args[1] = {ACTIVATE};
    assert(!skse_classic_control(&cpu, S_REG_CONTROL, args, 0x777, 0x30) && !handle_refs);

    /* Form deletion after the original call: this VM's rows for the handle go. */
    assert(op(S_REG_CONTROL, 0x30, ACTIVATE) && op(S_REG_CONTROL, 0x40, ACTIVATE));
    skyrim_classic_control_form_deleted(&cpu, 0x777, 0x100000030ull);
    assert(handle_refs == 2);
    skyrim_classic_control_form_deleted(&cpu, REGISTRY, 0x100000030ull);
    assert(handle_refs == 1 && string_refs[0] == 1);

    /* After a load, rows are dropped without the old handle policy; the strings are released. */
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    skyrim_classic_control_edge(&cpu, ACTIVATE, 1, 0.0f);  /* closed: nothing */
    skyrim_vm_epoch_after_load(1);
    sent_count = 0;
    skyrim_classic_control_edge(&cpu, ACTIVATE, 1, 0.0f);
    assert(!sent_count && !string_refs[0] && handle_refs == 1 && !control_bridge.head);
    return 0;
}
