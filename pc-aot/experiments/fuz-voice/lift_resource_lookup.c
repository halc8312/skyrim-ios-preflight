/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_resource_lookup.h"
#include "funcs_exe.h"
#include "lift_prelude.h"

#include <string.h>

enum {
    NAME_CAPACITY = 260, BSISTREAM_VTABLE = 0x0111eaf8,
    STREAM_FIELD = 4, VALID_FIELD = 8, NAME_FIELD = 12
};

bool skyrim_fuz_guest_lookup_layout(const SkyrimFuzGuestResources *resources, SkyrimFuzGuestLookupLayout *layout) {
    if (!resources || !resources->caller || !layout || !resources->nested_bytes) return false;
    const uint32_t live = resources->live_stack, limit = resources->stack_limit;
    const uint32_t name_space = SKYRIM_FUZ_GUEST_LOOKUP_GAP + SKYRIM_FUZ_GUEST_LOOKUP_NAME;
    const uint32_t frame_space = SKYRIM_FUZ_GUEST_LOOKUP_OBJECT + SKYRIM_FUZ_GUEST_LOOKUP_FRAME;
    if (live < limit || live >= resources->stack_base || live < name_space) return false;
    const uint32_t name = (live - name_space) & ~UINT32_C(15);
    if (name < limit || name - limit < frame_space) return false;
    const uint32_t frame = name - frame_space;
    if (frame - limit < resources->nested_bytes) return false;
    *layout = (SkyrimFuzGuestLookupLayout){name, name - SKYRIM_FUZ_GUEST_LOOKUP_OBJECT, frame};
    return true;
}

static SkyrimFuzAvailability lookup_stream(const CPU *caller, SkyrimFuzGuestLookupLayout at,
                                           const char *relative, size_t length) {
    memcpy(MEM + at.name, relative, length + 1);
    memset(MEM + at.object, 0, SKYRIM_FUZ_GUEST_LOOKUP_OBJECT);

    // BSIStream(const char *name, BSResource::Location *root = 0), thiscall.
    CPU cpu = *caller;
    cpu.r[1] = at.object;
    cpu.r[4] = at.frame;
    M32(at.frame) = 0;
    M32(at.frame + 4) = at.name;
    M32(at.frame + 8) = 0;
    f_B08630(&cpu);
    if (cpu.r[0] != at.object || cpu.r[4] != at.frame + 12 || M32(at.object) != BSISTREAM_VTABLE)
        return SKYRIM_FUZ_LOOKUP_ERROR;
    const bool named = M32(at.object + NAME_FIELD) != 0;
    const bool valid = M8(at.object + VALID_FIELD) != 0;
    const bool stream = M32(at.object + STREAM_FIELD) != 0;

    // Destroy in place. The object is on the guest stack, so flag 0.
    cpu = *caller;
    cpu.r[1] = at.object;
    cpu.r[4] = at.frame;
    M32(at.frame) = 0;
    M32(at.frame + 4) = 0;
    f_B08970(&cpu);
    if (cpu.r[4] != at.frame + 8 || !named || (valid && !stream)) return SKYRIM_FUZ_LOOKUP_ERROR;

    if (valid) return SKYRIM_FUZ_PRESENT;
    return stream ? SKYRIM_FUZ_LOOKUP_ERROR : SKYRIM_FUZ_ABSENT;
}

SkyrimFuzAvailability skyrim_fuz_guest_resource_lookup(void *opaque, const char *relative) {
    static _Thread_local bool active;
    const SkyrimFuzGuestResources *resources = opaque;
    SkyrimFuzGuestLookupLayout at;
    if (active || !relative || !skyrim_fuz_guest_lookup_layout(resources, &at)) return SKYRIM_FUZ_LOOKUP_ERROR;
    size_t length = 0;
    while (length < NAME_CAPACITY && relative[length]) ++length;
    if (!length || length == NAME_CAPACITY) return SKYRIM_FUZ_LOOKUP_ERROR;
    active = true;
    const SkyrimFuzAvailability result = lookup_stream(resources->caller, at, relative, length);
    active = false;
    return result;
}

bool skyrim_fuz_choose_with_guest_resources(const SkyrimFuzPolicy *configured, const SkyrimFuzLiftMemory *memory,
                                            const SkyrimFuzGuestResources *resources, uint32_t response_object,
                                            SkyrimFuzLiftChoice *choice) {
    if (choice) *choice = (SkyrimFuzLiftChoice){0};
    SkyrimFuzGuestLookupLayout at;
    if (!configured || !skyrim_fuz_guest_lookup_layout(resources, &at) ||
        resources->stack_base - resources->live_stack < SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT + SKYRIM_FUZ_LIFT_FRAME_BYTES)
        return false;
    SkyrimFuzGuestResources lookup = *resources;
    SkyrimFuzPolicy policy = *configured;
    policy.lookup = skyrim_fuz_guest_resource_lookup;
    policy.context = &lookup;
    return skyrim_fuz_apply_lifted_path_choice(&policy, memory, lookup.live_stack, response_object, choice);
}
