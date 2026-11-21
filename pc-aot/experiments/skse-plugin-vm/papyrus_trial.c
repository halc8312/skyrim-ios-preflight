/* SPDX-License-Identifier: GPL-3.0-only */
#include "papyrus_trial.h"
#include <string.h>

bool skse_papyrus_trial_interface(uint8_t *bytes, size_t size, uint32_t register_service) {
    if (!bytes || size < SKSE_CLASSIC_PAPYRUS_TABLE_BYTES || !register_service) return false;
    const uint8_t table[SKSE_CLASSIC_PAPYRUS_TABLE_BYTES] = {
        SKSE_CLASSIC_PAPYRUS_VERSION, 0, 0, 0,
        (uint8_t)register_service, (uint8_t)(register_service >> 8),
        (uint8_t)(register_service >> 16), (uint8_t)(register_service >> 24)
    };
    memcpy(bytes, table, sizeof table);
    return true;
}

SksePapyrusTrialStatus skse_papyrus_trial_begin(SksePapyrusTrial *trial, uint32_t epoch,
                                               SksePapyrusTrialOps ops, void *context) {
    if (!trial || !epoch || !ops.owns_callback || !ops.owns_registry || !ops.invoke_cdecl)
        return SKSE_PAPYRUS_TRIAL_ARGUMENT;
    if (trial->epoch) return SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER;
    *trial = (SksePapyrusTrial){.ops = ops, .context = context, .epoch = epoch};
    return SKSE_PAPYRUS_TRIAL_OK;
}

SksePapyrusTrialStatus skse_papyrus_trial_register(SksePapyrusTrial *trial, uint32_t owner,
                                                  uint32_t address) {
    if (!trial || !trial->epoch || owner == UINT32_MAX || !address)
        return SKSE_PAPYRUS_TRIAL_ARGUMENT;
    if (trial->phase != SKSE_PAPYRUS_COLLECTING) return SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER;
    if (!trial->ops.owns_callback(trial->context, owner, address))
        return SKSE_PAPYRUS_TRIAL_CALLBACK_NOT_OWNED;

    for (size_t i = 0; i < trial->count; ++i) {
        const SksePapyrusTrialCallback entry = trial->callbacks[i];
        if (entry.owner != owner) continue;
        if (entry.address == address) return SKSE_PAPYRUS_TRIAL_OK;
        return SKSE_PAPYRUS_TRIAL_OWNER_ALREADY_REGISTERED;
    }
    if (trial->count == SKSE_PAPYRUS_TRIAL_MAX_OWNERS) return SKSE_PAPYRUS_TRIAL_FULL;
    trial->callbacks[trial->count++] = (SksePapyrusTrialCallback){owner, address};
    return SKSE_PAPYRUS_TRIAL_OK;
}

SksePapyrusTrialReport skse_papyrus_trial_dispatch(SksePapyrusTrial *trial, uint32_t epoch,
                                                   uint32_t registry) {
    if (!trial || !trial->epoch || !epoch || !registry || registry % 4)
        return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_ARGUMENT, 0, UINT32_MAX};
    if (trial->phase != SKSE_PAPYRUS_COLLECTING)
        return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER, 0, UINT32_MAX};
    if (epoch != trial->epoch || !trial->ops.owns_registry(trial->context, epoch, registry))
        return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED, 0, UINT32_MAX};
    trial->phase = SKSE_PAPYRUS_SEALED;
    trial->registry = registry;
    if (!trial->count)
        return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_NO_CALLBACK, 0, UINT32_MAX};

    for (size_t i = 0; i < trial->count; ++i) {
        const SksePapyrusTrialCallback entry = trial->callbacks[i];
        if (!trial->ops.owns_callback(trial->context, entry.owner, entry.address))
            return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_CALLBACK_NOT_OWNED, 0, entry.owner};
    }
    for (size_t i = 0; i < trial->count; ++i) {
        const SksePapyrusTrialCallback entry = trial->callbacks[i];
        uint32_t eax = 0;
        trial->active_owner = entry.owner;
        trial->phase = SKSE_PAPYRUS_REGISTERING;
        const bool completed = trial->ops.invoke_cdecl(trial->context, entry.owner, entry.address, &registry, 1, &eax);
        trial->phase = SKSE_PAPYRUS_SEALED;
        if (!completed)
            return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_CALLBACK_FAULT, i + 1, entry.owner};
        const uint8_t returned = (uint8_t)eax;
        if (returned == 0)
            return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_CALLBACK_DECLINED, i + 1, entry.owner};
        if (returned != 1)
            return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_CALLBACK_BAD_BOOL, i + 1, entry.owner};
    }
    trial->phase = SKSE_PAPYRUS_REGISTERED;
    return (SksePapyrusTrialReport){SKSE_PAPYRUS_TRIAL_OK, trial->count, UINT32_MAX};
}

static bool read_owned_run(const SksePapyrusTrial *trial, SksePapyrusTrialNativeOps ops,
                           uint32_t owner, uint32_t object, uint32_t *vtable, uint32_t *run) {
    const uint32_t run_offset = SKSE_CLASSIC_PAPYRUS_RUN_SLOT * 4;
    return ops.read_u32(trial->context, owner, object, vtable) && *vtable && *vtable % 4 == 0 &&
           *vtable <= UINT32_MAX - run_offset - 3 &&
           ops.read_u32(trial->context, owner, *vtable + run_offset, run) && *run &&
           trial->ops.owns_callback(trial->context, owner, *run);
}

SksePapyrusTrialStatus skse_papyrus_trial_capture_int0(SksePapyrusTrial *trial, uint32_t registry,
                                                      uint32_t object, SksePapyrusTrialNativeOps ops,
                                                      SksePapyrusTrialNative *out) {
    if (!trial || !trial->epoch || !registry || !object || object % 4 || !out || !ops.read_u32 ||
        !ops.owns_frame || !ops.invoke_thiscall)
        return SKSE_PAPYRUS_TRIAL_ARGUMENT;
    if (trial->phase != SKSE_PAPYRUS_REGISTERING) return SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER;
    if (trial->registry != registry || !trial->ops.owns_registry(trial->context, trial->epoch, registry))
        return SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED;
    if (out->trial) return SKSE_PAPYRUS_TRIAL_NATIVE_ALREADY_CAPTURED;

    SksePapyrusTrialNative candidate = {
        .trial = trial, .ops = ops, .epoch = trial->epoch, .owner = trial->active_owner, .object = object
    };
    if (!read_owned_run(trial, ops, candidate.owner, object, &candidate.vtable, &candidate.run))
        return SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED;
    *out = candidate;
    return SKSE_PAPYRUS_TRIAL_OK;
}

SksePapyrusTrialStatus skse_papyrus_trial_run_int0(SksePapyrusTrial *trial,
                                                  const SksePapyrusTrialNative *native,
                                                  uint32_t stack_id, uint32_t result, uint32_t state,
                                                  uint32_t *int_bits) {
    if (!trial || !trial->epoch || !native || !int_bits || !result || result % 4 || result > UINT32_MAX - 7 ||
        !state || state % 4)
        return SKSE_PAPYRUS_TRIAL_ARGUMENT;
    if (trial->phase != SKSE_PAPYRUS_REGISTERED) return SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER;
    if (native->trial != trial || native->epoch != trial->epoch) return SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED;
    const uint32_t owner = native->owner;
    const SksePapyrusTrialNativeOps ops = native->ops;
    void *context = trial->context;
    if (!trial->ops.owns_registry(context, trial->epoch, trial->registry))
        return SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED;
    uint32_t vtable, run;
    if (!read_owned_run(trial, ops, owner, native->object, &vtable, &run) ||
        native->vtable != vtable || native->run != run)
        return SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED;
    uint32_t type, bits;
    if (!ops.owns_frame(context, owner, trial->epoch, stack_id, result, state) ||
        !ops.read_u32(context, owner, result, &type) || type != 0 ||
        !ops.read_u32(context, owner, result + 4, &bits) || bits != 0)
        return SKSE_PAPYRUS_TRIAL_FRAME_REFUSED;

    const uint32_t args[] = {0, trial->registry, stack_id, result, state};
    uint32_t eax = 0;
    trial->phase = SKSE_PAPYRUS_RUNNING;
    const bool completed = ops.invoke_thiscall(context, owner, run, native->object, args, 5, &eax);
    trial->phase = SKSE_PAPYRUS_SEALED;
    if (!completed) return SKSE_PAPYRUS_TRIAL_NATIVE_FAULT;
    const uint8_t returned = (uint8_t)eax;
    if (returned == 0) return SKSE_PAPYRUS_TRIAL_CALLBACK_DECLINED;
    if (returned != 1) return SKSE_PAPYRUS_TRIAL_CALLBACK_BAD_BOOL;
    if (!ops.read_u32(context, owner, result, &type) || type != SKSE_CLASSIC_PAPYRUS_INT ||
        !ops.read_u32(context, owner, result + 4, &bits))
        return SKSE_PAPYRUS_TRIAL_RESULT_REFUSED;
    *int_bits = bits;
    trial->phase = SKSE_PAPYRUS_REGISTERED;
    return SKSE_PAPYRUS_TRIAL_OK;
}
