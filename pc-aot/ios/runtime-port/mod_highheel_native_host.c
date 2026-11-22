/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_highheel_native_host.h"
#include "mod_highheel_class_host.h"
#include "mod_highheel_effect_host.h"
#include "PcGuestABI.h"
#include "classic_papyrus_values.h"

#include <string.h>

extern uint8_t *MEM;
extern CPU *rt_current_cpu(void);
extern uint32_t gmalloc(uint32_t);
extern void gfree(uint32_t);
extern uint32_t rt_invoke(CPU *, uint32_t, int, const uint32_t *);
extern uint32_t skse_member(CPU *, uint32_t, uint32_t, int, const uint32_t *);
extern uint32_t skse_virtual(CPU *, uint32_t, unsigned, int, const uint32_t *);

enum {
    RECEIPT = 1, KEEP = 1, STOP = 2, FIRST_MAPPED = 0x10000,
    IDENTIFIER_GET_HANDLE = 0xc307c0, IDENTIFIER_RELEASE = 0xc30720,
    IDENTIFIER_DESTROY = 0xc30e90, FIND_BOUND = 0x1b,
    UNPACK_REFERENCE = 0x8d9390, LOOKUP_FORM = 0x451a30
};

static const ClassicValueFunction functions[] = {
    {"hdtHighHeelEffect", "KeepUpdate", KEEP, CLASSIC_VALUES_NONE, 2,
     {CLASSIC_VALUES_OBJECT_REFERENCE, CLASSIC_VALUES_FLOAT}},
    {"hdtHighHeelEffect", "StopUpdate", STOP, CLASSIC_VALUES_NONE, 1,
     {CLASSIC_VALUES_OBJECT_REFERENCE}},
};

static SkyrimHighHeelQueueUpdate queue_update;
static void *queue_context;
static _Thread_local struct {
    CPU *owner;
    ClassicValueFrame frame;
    SkyrimHighHeelUpdate update;
    bool ready, rechecked, committing;
} call;

static bool read_guest(void *unused, uint32_t address, void *bytes, size_t size) {
    (void)unused;
    if (!MEM || address < FIRST_MAPPED || size > UINT64_C(0x100000000) - address) return false;
    memcpy(bytes, MEM + address, size);
    return true;
}

static bool copy_guest(void *unused, CPU *cpu, uint32_t address, void *bytes, size_t size) {
    (void)unused;
    return cpu && cpu == rt_current_cpu() && read_guest(NULL, address, bytes, size);
}

typedef struct {
    CPU *cpu;
    SkyrimVmScope scope;
    uint32_t effect_class;
    uint64_t handle;
} BorrowedSelf;

static bool visit_self(void *context, uint32_t value_address) {
    BorrowedSelf *self = context;
    uint32_t words[2], identifier_type, name;
    if (!read_guest(NULL, value_address, words, sizeof words) || words[0] != self->effect_class ||
        words[1] < FIRST_MAPPED || (words[1] & 3u) || words[1] > UINT32_MAX - 0x1bu ||
        !read_guest(NULL, words[1] + 4, &identifier_type, 4) ||
        identifier_type != self->effect_class || self->effect_class > UINT32_MAX - 7u ||
        !read_guest(NULL, self->effect_class + 4, &name, 4) || name < FIRST_MAPPED) return false;

    CPU *cpu = self->cpu;
    const CPU saved = *cpu;
    cpu->r[1] = words[1];
    const uint32_t low = rt_invoke(cpu, IDENTIFIER_GET_HANDLE, 0, NULL), high = cpu->r[2];
    *cpu = saved;
    if (!low || (high >> 16) != 2u || !(uint16_t)high) return false;

    /* Confirm the snapshot with the registry's current binding for the
     * verified class. The stock query lends a reference even on a mismatch. */
    const uint32_t output = gmalloc(4);
    if (output < FIRST_MAPPED || output > UINT32_MAX - 3u || (output & 3u)) {
        if (output) gfree(output);
        return false;
    }
    memset(MEM + output, 0, 4);
    const uint32_t args[] = {low, high, name, output};
    const bool found = (uint8_t)skse_virtual(cpu, self->scope.registry, FIND_BOUND, 4, args) != 0;
    uint32_t retained;
    memcpy(&retained, MEM + output, 4);
    gfree(output);
    if (retained < FIRST_MAPPED || (retained & 3u)) return false;
    if (!skse_member(cpu, IDENTIFIER_RELEASE, retained, 0, NULL))
        skse_member(cpu, IDENTIFIER_DESTROY, retained, 0, NULL);
    if (!found || retained != words[1]) return false;
    self->handle = ((uint64_t)high << 32) | low;
    return true;
}

static bool owns_frame(void *unused, CPU *cpu, uint64_t receipt, const ClassicValueFrame *frame) {
    (void)unused;
    call.rechecked = false;
    SkyrimVmScope scope;
    if (!cpu || cpu != rt_current_cpu() || !frame || frame->object != cpu->r[1] || frame->sp != cpu->r[4] ||
        receipt != RECEIPT || !skyrim_vm_scope_current(&scope) || scope.registry != frame->registry) return false;
    if (call.owner && (call.owner != cpu || call.update.scope.epoch != scope.epoch ||
                       call.update.scope.registry != scope.registry || !classic_value_same_frame(&call.frame, frame)))
        return false;
    HighHeelNativeBindings bindings;
    if (!skyrim_highheel_class_snapshot(scope, &bindings) || !bindings.keep_update || !bindings.stop_update)
        return false;
    BorrowedSelf borrowed = {.cpu = cpu, .scope = scope, .effect_class = bindings.effect_class};
    if (!highheel_visit_native_self(&bindings, scope.epoch, cpu, rt_current_cpu(), frame->object, frame->sp,
                                    read_guest, NULL, visit_self, &borrowed)) return false;
    if (call.owner) {
        const bool same = call.update.handle == borrowed.handle;
        call.rechecked = same && call.ready;
        return same;
    }

    call.owner = cpu;
    call.frame = *frame;
    call.update = (SkyrimHighHeelUpdate){.scope = scope, .handle = borrowed.handle,
                                       .actor = (uint32_t)borrowed.handle,
                                       .keep = frame->object == bindings.keep_update};
    return true;
}

static bool copy_target(void *unused, CPU *cpu, const ClassicValueFrame *frame, uint32_t address, uint32_t *actor) {
    (void)unused;
    if (!cpu || cpu != call.owner || !frame || !actor || !classic_value_same_frame(frame, &call.frame))
        return false;
    const uint32_t args[] = {address, frame->registry};
    const uint32_t target = skse_member(cpu, UNPACK_REFERENCE, 0, 2, args);
    if (!target || target != skse_member(cpu, LOOKUP_FORM, 0, 1, &call.update.actor)) return false;
    *actor = call.update.actor;
    return true;
}

static bool invoke(void *unused, uint64_t receipt, uint32_t method, const ClassicValue *args, size_t count,
                   ClassicValue *out, char *text, size_t capacity) {
    (void)unused; (void)text; (void)capacity;
    const bool keep = call.update.keep;
    if (receipt != RECEIPT || !call.owner || call.ready || !args || !out || method != (keep ? KEEP : STOP) ||
        count != (keep ? 2u : 1u) || args[0].type != CLASSIC_VALUES_OBJECT_REFERENCE ||
        args[0].bits != call.update.actor || (keep && args[1].type != CLASSIC_VALUES_FLOAT)) return false;
    const uint32_t magnitude = keep ? args[1].bits : 0;
    if ((magnitude & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) return false;
    if (!skyrim_highheel_effect_matches(call.update.scope, call.update.handle, call.update.actor, keep, magnitude))
        return false;
    call.update.magnitude = magnitude;
    call.ready = true;
    *out = (ClassicValue){.type = CLASSIC_VALUES_NONE};
    return true;
}

static bool enqueue(void *unused) {
    (void)unused;
    return queue_update(queue_context, &call.update);
}

static bool commit(void *unused, uint64_t receipt, uint32_t method) {
    (void)unused;
    if (receipt != RECEIPT || !call.ready || !call.rechecked || method != (call.update.keep ? KEEP : STOP) ||
        !skyrim_vm_commit_begin(&call.update.scope)) return false;
    const SkyrimHighHeelUpdate *update = &call.update;
    if (!skyrim_highheel_effect_commit_if_matches(update->scope, update->handle, update->actor, update->keep,
                                                  update->magnitude, enqueue, NULL)) {
        skyrim_vm_commit_end();
        return false;
    }
    call.committing = true;
    return true;
}

static void finished(void *unused, uint64_t receipt) {
    (void)unused; (void)receipt;
    if (call.committing) skyrim_vm_commit_end();
    memset(&call, 0, sizeof call);
}

bool skyrim_highheel_native_prepare(SkyrimHighHeelQueueUpdate queue, void *context) {
    if (!queue || queue_update) return false;
    const ClassicValueRequest request = {.epoch = RECEIPT, .functions = functions,
        .count = sizeof functions / sizeof functions[0], .read_guest = copy_guest, .owns_frame = owns_frame,
        .copy_form_id = copy_target, .invoke = invoke, .commit = commit, .finished = finished};
    if (!skse_classic_values_request_for_provider(CLASSIC_VALUE_PROVIDER_HIGHHEELS, &request)) return false;
    queue_update = queue;
    queue_context = context;
    return true;
}

bool skyrim_highheel_native_retire(void) {
    if (!queue_update || !skse_classic_values_retire_for_provider(CLASSIC_VALUE_PROVIDER_HIGHHEELS, RECEIPT))
        return false;
    queue_update = NULL;
    queue_context = NULL;
    return true;
}
