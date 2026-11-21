/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_resource_lookup.h"
#include "funcs_exe.h"
#include "lift_prelude.h"

#include <string.h>

enum {
    NAME_CAPACITY = 260, BSISTREAM_VTABLE = 0x0111eaf8,
    STREAM_FIELD = 4, VALID_FIELD = 8, NAME_FIELD = 12
};

bool skyrim_fuz_guest_lookup_layout(uint32_t live_stack, SkyrimFuzGuestLookupLayout *layout) {
    const uint32_t used = SKYRIM_FUZ_GUEST_LOOKUP_GAP + SKYRIM_FUZ_GUEST_LOOKUP_NAME +
                          SKYRIM_FUZ_GUEST_LOOKUP_OBJECT + SKYRIM_FUZ_GUEST_LOOKUP_FRAME + 16;
    if (!layout || live_stack < SKYRIM_FUZ_GUEST_LOOKUP_MIN_STACK + used) return false;
    layout->name = (live_stack - SKYRIM_FUZ_GUEST_LOOKUP_GAP - SKYRIM_FUZ_GUEST_LOOKUP_NAME) & ~UINT32_C(15);
    layout->object = layout->name - SKYRIM_FUZ_GUEST_LOOKUP_OBJECT;
    layout->frame = layout->object - SKYRIM_FUZ_GUEST_LOOKUP_FRAME;
    return true;
}

SkyrimFuzAvailability skyrim_fuz_guest_resource_lookup(void *opaque, const char *relative) {
    const SkyrimFuzGuestResources *resources = opaque;
    SkyrimFuzGuestLookupLayout at;
    if (!resources || !resources->caller || !relative ||
        !skyrim_fuz_guest_lookup_layout(resources->live_stack, &at))
        return SKYRIM_FUZ_LOOKUP_ERROR;
    size_t length = 0;
    while (length < NAME_CAPACITY && relative[length]) ++length;
    if (!length || length == NAME_CAPACITY) return SKYRIM_FUZ_LOOKUP_ERROR;

    memcpy(MEM + at.name, relative, length + 1);
    memset(MEM + at.object, 0, SKYRIM_FUZ_GUEST_LOOKUP_OBJECT);

    // BSIStream(const char *name, BSResource::Location *root = 0), thiscall.
    CPU cpu = {0};
    cpu.fs_base = resources->caller->fs_base;
    cpu.r[1] = at.object;
    cpu.r[4] = at.frame;
    M32(at.frame) = 0;
    M32(at.frame + 4) = at.name;
    M32(at.frame + 8) = 0;
    f_B08630(&cpu);
    if (cpu.r[0] != at.object || cpu.r[4] != at.frame + 12) return SKYRIM_FUZ_LOOKUP_ERROR;
    // Anything but the reviewed object shape keeps the original path.
    const bool reviewed = M32(at.object) == BSISTREAM_VTABLE && M32(at.object + NAME_FIELD);
    const bool valid = M8(at.object + VALID_FIELD) != 0;
    const bool stream = M32(at.object + STREAM_FIELD) != 0;

    // Destroy in place. The object is on the guest stack, so flag 0.
    cpu = (CPU){0};
    cpu.fs_base = resources->caller->fs_base;
    cpu.r[1] = at.object;
    cpu.r[4] = at.frame;
    M32(at.frame) = 0;
    M32(at.frame + 4) = 0;
    f_B08970(&cpu);
    if (cpu.r[4] != at.frame + 8 || !reviewed) return SKYRIM_FUZ_LOOKUP_ERROR;

    if (valid) return SKYRIM_FUZ_PRESENT;
    return stream ? SKYRIM_FUZ_LOOKUP_ERROR : SKYRIM_FUZ_ABSENT;
}

bool skyrim_fuz_choose_with_guest_resources(const SkyrimFuzPolicy *configured, const SkyrimFuzLiftMemory *memory,
                                            const struct CPU *caller, uint32_t argument_slot,
                                            uint32_t response_object, SkyrimFuzLiftChoice *choice) {
    if (choice) *choice = (SkyrimFuzLiftChoice){0};
    if (!configured || !caller) return false;
    SkyrimFuzGuestResources resources = {.caller = caller, .live_stack = argument_slot};
    SkyrimFuzPolicy policy = *configured;
    policy.lookup = skyrim_fuz_guest_resource_lookup;
    policy.context = &resources;
    return skyrim_fuz_apply_lifted_path_choice(&policy, memory, argument_slot, response_object, choice);
}
