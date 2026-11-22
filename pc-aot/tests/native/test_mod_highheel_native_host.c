/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86.h"
#include "classic_papyrus_values.h"
#include "mod_highheel_class_host.h"
#include "mod_highheel_effect_host.h"
#include "mod_highheel_native_host.h"

#include <assert.h>
#include <string.h>

enum {
    REGISTRY = 0x10100, STACK = 0x10200, FRAME = 0x10300, CALLER = 0x10400,
    SP = 0x10500, ARGUMENTS = 0x10600, EFFECT_CLASS = 0x10700,
    CLASS_NAME = 0x10800, SELF = 0x10900, OTHER_SELF = 0x10a00,
    SCRATCH = 0x10b00, KEEP = 0x11100, STOP = 0x11200,
    MOVE_UP = 0x11300, ON_UPDATE = 0x11400, MOVE_DOWN = 0x11500,
    TARGET = 0x11600, OTHER_TARGET = 0x11700, STACK_ID = 8,
    ACTOR = 0x14, EFFECT_HI = 0x2f223, MAGNITUDE = 0x40e80000
};

static uint8_t guest[0x12000];
uint8_t *MEM = guest;
static CPU owner, foreign;
static ClassicValueRequest request;
static HighHeelNativeBindings bindings;
static SkyrimHighHeelUpdate submitted;
static bool queue_allowed = true, commit_allowed = true, scratch_live;
static bool did_retire;
static uint32_t resolved_target = TARGET, bound_self = SELF;
static unsigned acquisitions, releases, destroys, queued;

static void put(uint32_t address, uint32_t value) {
    assert(address <= sizeof guest - sizeof value);
    memcpy(guest + address, &value, sizeof value);
}

CPU *rt_current_cpu(void) { return &owner; }

bool skse_classic_values_request_for_provider(ClassicValueProvider provider, const ClassicValueRequest *src) {
    assert(provider == CLASSIC_VALUE_PROVIDER_HIGHHEELS);
    if (request.invoke) return false;
    request = *src;
    return true;
}

bool skse_classic_values_retire_for_provider(ClassicValueProvider provider, uint64_t receipt) {
    assert(provider == CLASSIC_VALUE_PROVIDER_HIGHHEELS && receipt == request.epoch);
    if (did_retire) return false;
    did_retire = true;
    return true;
}

bool skse_classic_values_matches_published_object(ClassicValueProvider provider, uint64_t receipt,
                                                  uint32_t registry, uint32_t method, uint32_t object) {
    return provider == CLASSIC_VALUE_PROVIDER_HIGHHEELS && receipt == request.epoch && registry == REGISTRY &&
           ((method == 1 && object == KEEP) || (method == 2 && object == STOP));
}

bool skyrim_highheel_class_snapshot(SkyrimVmScope scope, HighHeelNativeBindings *out) {
    if (scope.epoch != bindings.epoch || scope.registry != bindings.registry) return false;
    *out = bindings;
    return true;
}

static bool matching_lifetime(SkyrimVmScope scope, uint64_t handle, uint32_t actor, bool keep,
                              uint32_t magnitude) {
    return scope.epoch == 1 && scope.registry == REGISTRY &&
           handle == ((uint64_t)EFFECT_HI << 32 | ACTOR) && actor == ACTOR &&
           (!keep || magnitude == MAGNITUDE);
}

bool skyrim_highheel_effect_matches(SkyrimVmScope scope, uint64_t handle, uint32_t actor,
                                     bool keep, uint32_t magnitude) {
    return matching_lifetime(scope, handle, actor, keep, magnitude);
}

bool skyrim_highheel_effect_commit_if_matches(SkyrimVmScope scope, uint64_t handle, uint32_t actor, bool keep,
                                               uint32_t magnitude, bool (*accept)(void *), void *context) {
    return commit_allowed && matching_lifetime(scope, handle, actor, keep, magnitude) && accept(context);
}

uint32_t gmalloc(uint32_t size) {
    assert(!scratch_live && size == 4);
    scratch_live = true;
    return SCRATCH;
}

void gfree(uint32_t address) {
    assert(scratch_live && address == SCRATCH);
    scratch_live = false;
}

uint32_t rt_invoke(CPU *cpu, uint32_t address, int count, const uint32_t *args) {
    assert(cpu == &owner && address == 0xc307c0 && !count && !args && cpu->r[1] == SELF);
    cpu->r[0] = ACTOR;
    cpu->r[2] = EFFECT_HI;
    return ACTOR;
}

uint32_t skse_virtual(CPU *cpu, uint32_t self, unsigned slot, int count, const uint32_t *args) {
    assert(cpu == &owner && self == REGISTRY && slot == 0x1b && count == 4 && scratch_live);
    assert(args[0] == ACTOR && args[1] == EFFECT_HI && args[2] == CLASS_NAME && args[3] == SCRATCH);
    if (!bound_self) return 0;
    ++acquisitions;
    put(SCRATCH, bound_self);
    return 1;
}

uint32_t skse_member(CPU *cpu, uint32_t method, uint32_t self, int count, const uint32_t *args) {
    assert(cpu == &owner);
    switch (method) {
    case 0xc30720:
        assert((self == SELF || self == OTHER_SELF) && !count && !args && releases < acquisitions);
        ++releases;
        return self == SELF ? 1 : 0;
    case 0xc30e90:
        assert(self == OTHER_SELF && !count && !args);
        ++destroys;
        return 0;
    case 0x8d9390:
        assert(!self && count == 2 && args[0] == ARGUMENTS && args[1] == REGISTRY);
        return resolved_target;
    case 0x451a30:
        assert(!self && count == 1 && args[0] == ACTOR);
        return TARGET;
    default:
        assert(!"unexpected guest function");
        return 0;
    }
}

static bool queue(void *context, const SkyrimHighHeelUpdate *update) {
    assert(context == &submitted && update);
    if (!queue_allowed) return false;
    submitted = *update;
    ++queued;
    return true;
}

static ClassicValueFrame frame(uint32_t object, uint32_t caller) {
    owner.r[1] = object;
    owner.r[4] = SP;
    put(SP, CLASSIC_VALUES_RUN_RETURN);
    put(SP + 4, SP + CLASSIC_VALUES_BASE_OFFSET);
    put(SP + 8, REGISTRY);
    put(SP + 12, STACK_ID);
    put(SP + 16, SP + CLASSIC_VALUES_RESULT_OFFSET);
    put(SP + 20, FRAME);
    put(SP + CLASSIC_VALUES_BASE_OFFSET, 0);
    put(FRAME, STACK);
    put(FRAME + 4, CALLER);
    put(FRAME + 8, object);
    put(STACK + 0x30, FRAME);
    put(STACK + 0x44, STACK_ID);
    put(CALLER, STACK);
    put(CALLER + 8, caller);
    put(CALLER + 0x14, EFFECT_CLASS);
    put(CALLER + 0x18, SELF);
    return (ClassicValueFrame){.object = object, .sp = SP, .base = SP + CLASSIC_VALUES_BASE_OFFSET,
                              .registry = REGISTRY, .stack_id = STACK_ID,
                              .result = SP + CLASSIC_VALUES_RESULT_OFFSET, .state = FRAME,
                              .argument_list = ARGUMENTS};
}

enum RunDisruption { RUN_INTACT, RUN_SKIP_RECHECK, RUN_REBIND, RUN_REVERT };

static bool run(uint32_t object, uint32_t caller, uint32_t magnitude, enum RunDisruption disruption) {
    const ClassicValueFrame native = frame(object, caller);
    const CPU original = owner;
    const bool keep = object == KEEP;
    const uint32_t method = keep ? 1 : 2;
    uint32_t actor = 0;
    bool accepted = request.owns_frame(request.context, &owner, request.epoch, &native) &&
                    request.copy_form_id(request.context, &owner, &native, ARGUMENTS, &actor);
    if (accepted) {
        const ClassicValue args[] = {
            {.type = CLASSIC_VALUES_OBJECT_REFERENCE, .bits = actor},
            {.type = CLASSIC_VALUES_FLOAT, .bits = magnitude}
        };
        ClassicValue output = {.type = CLASSIC_VALUES_BOOL};
        accepted = request.invoke(request.context, request.epoch, method, args, keep ? 2 : 1, &output, NULL, 0);
        if (accepted) {
            assert(output.type == CLASSIC_VALUES_NONE && !output.bits && !output.string);
            const uint32_t previous_self = bound_self;
            if (disruption == RUN_REBIND) bound_self = OTHER_SELF;
            if (disruption != RUN_SKIP_RECHECK)
                accepted = request.owns_frame(request.context, &owner, request.epoch, &native);
            bound_self = previous_self;
        }
        if (accepted && disruption == RUN_REVERT) skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
        if (accepted) accepted = request.commit(request.context, request.epoch, method);
        if (!accepted && request.discard) request.discard(request.context, request.epoch);
    }
    request.finished(request.context, request.epoch);
    assert(!scratch_live && acquisitions == releases);
    assert(memcmp(&original, &owner, sizeof owner) == 0);
    return accepted;
}

int main(void) {
    assert(!skyrim_highheel_native_prepare(NULL, &submitted));
    assert(skyrim_highheel_native_prepare(queue, &submitted));
    assert(request.epoch == 1 && request.count == 2);
    for (size_t i = 0; i < request.count; ++i) {
        const ClassicValueFunction *fn = &request.functions[i];
        assert(strcmp(fn->class_name, "hdtHighHeelEffect") == 0 &&
               strcmp(fn->name, i ? "StopUpdate" : "KeepUpdate") == 0);
        assert(fn->id == i + 1 && fn->count == (i ? 1u : 2u) && fn->result == CLASSIC_VALUES_NONE);
        assert(fn->arguments[0] == CLASSIC_VALUES_OBJECT_REFERENCE);
        if (!i) assert(fn->arguments[1] == CLASSIC_VALUES_FLOAT);
    }
    bindings = (HighHeelNativeBindings){.epoch = 1, .registry = REGISTRY, .effect_class = EFFECT_CLASS,
        .keep_update = KEEP, .stop_update = STOP, .move_up = MOVE_UP, .on_update = ON_UPDATE, .move_down = MOVE_DOWN};
    put(EFFECT_CLASS + 4, CLASS_NAME);
    put(SELF + 4, EFFECT_CLASS);
    memcpy(guest + CLASS_NAME, "hdtHighHeelEffect", sizeof "hdtHighHeelEffect");
    skyrim_vm_epoch_registered(REGISTRY);

    const ClassicValueFrame native = frame(KEEP, MOVE_UP);
    assert(!request.owns_frame(request.context, &foreign, request.epoch, &native));
    request.finished(request.context, request.epoch);
    assert(!run(KEEP, MOVE_DOWN, MAGNITUDE, RUN_INTACT) && !queued);
    assert(!run(STOP, ON_UPDATE, MAGNITUDE, RUN_INTACT) && !queued);

    bound_self = 0;
    assert(!run(KEEP, MOVE_UP, MAGNITUDE, RUN_INTACT) && !queued && !acquisitions);
    bound_self = OTHER_SELF;
    assert(!run(KEEP, MOVE_UP, MAGNITUDE, RUN_INTACT) && !queued && destroys == 1);
    bound_self = SELF;
    resolved_target = OTHER_TARGET;
    assert(!run(KEEP, MOVE_UP, MAGNITUDE, RUN_INTACT) && !queued);
    resolved_target = TARGET;
    assert(!run(KEEP, MOVE_UP, MAGNITUDE + 1, RUN_INTACT) && !queued);

    assert(run(KEEP, MOVE_UP, MAGNITUDE, RUN_INTACT) && queued == 1);
    assert(submitted.scope.epoch == 1 && submitted.scope.registry == REGISTRY &&
           submitted.handle == ((uint64_t)EFFECT_HI << 32 | ACTOR) &&
           submitted.actor == ACTOR && submitted.keep && submitted.magnitude == MAGNITUDE);
    assert(!run(STOP, MOVE_DOWN, 0, RUN_SKIP_RECHECK) && queued == 1 && submitted.keep);
    assert(!run(STOP, MOVE_DOWN, 0, RUN_REBIND) && queued == 1 && submitted.keep && destroys == 2);
    commit_allowed = false;
    assert(!run(STOP, MOVE_DOWN, 0, RUN_INTACT) && queued == 1 && submitted.keep);
    commit_allowed = true;
    queue_allowed = false;
    assert(!run(STOP, MOVE_DOWN, 0, RUN_INTACT) && queued == 1 && submitted.keep);
    queue_allowed = true;
    assert(run(STOP, MOVE_DOWN, 0, RUN_INTACT) && queued == 2 && !submitted.keep && !submitted.magnitude);
    assert(!run(KEEP, ON_UPDATE, MAGNITUDE, RUN_REVERT) && queued == 2 && !submitted.keep);
    skyrim_vm_epoch_after_load(1);
    assert(!run(KEEP, MOVE_UP, MAGNITUDE, RUN_INTACT) && queued == 2);
    assert(skyrim_highheel_native_retire());
}
