/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_PAPYRUS_TRIAL_H
#define SKYRIM_EXPERIMENT_SKSE_PAPYRUS_TRIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SKSE_CLASSIC_PAPYRUS_ID = 2,
    SKSE_CLASSIC_PAPYRUS_VERSION = 1,
    SKSE_CLASSIC_PAPYRUS_TABLE_BYTES = 8,
    SKSE_CLASSIC_PAPYRUS_REGISTER_SLOT = 0x17,
    SKSE_CLASSIC_PAPYRUS_RUN_SLOT = 22,
    SKSE_CLASSIC_PAPYRUS_INT = 3,
    SKSE_PAPYRUS_TRIAL_MAX_OWNERS = 4
};

typedef enum {
    SKSE_PAPYRUS_TRIAL_OK,
    SKSE_PAPYRUS_TRIAL_ARGUMENT,
    SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER,
    SKSE_PAPYRUS_TRIAL_CALLBACK_NOT_OWNED,
    SKSE_PAPYRUS_TRIAL_OWNER_ALREADY_REGISTERED,
    SKSE_PAPYRUS_TRIAL_FULL,
    SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED,
    SKSE_PAPYRUS_TRIAL_NO_CALLBACK,
    SKSE_PAPYRUS_TRIAL_CALLBACK_FAULT,
    SKSE_PAPYRUS_TRIAL_CALLBACK_DECLINED,
    SKSE_PAPYRUS_TRIAL_CALLBACK_BAD_BOOL,
    SKSE_PAPYRUS_TRIAL_NATIVE_ALREADY_CAPTURED,
    SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED,
    SKSE_PAPYRUS_TRIAL_FRAME_REFUSED,
    SKSE_PAPYRUS_TRIAL_NATIVE_FAULT,
    SKSE_PAPYRUS_TRIAL_RESULT_REFUSED
} SksePapyrusTrialStatus;

typedef struct {
    /* Owner IDs come from the session, never from untrusted DLL arguments. */
    bool (*owns_callback)(void *context, uint32_t owner, uint32_t address);
    bool (*owns_registry)(void *context, uint32_t epoch, uint32_t address);
    /* Execute interpreted cdecl code with the caller's shared work budget. */
    bool (*invoke_cdecl)(void *context, uint32_t owner, uint32_t address,
                        const uint32_t *args, size_t count, uint32_t *eax);
} SksePapyrusTrialOps;

typedef struct {
    uint32_t owner, address;
} SksePapyrusTrialCallback;

typedef enum {
    SKSE_PAPYRUS_COLLECTING, SKSE_PAPYRUS_SEALED, SKSE_PAPYRUS_REGISTERING,
    SKSE_PAPYRUS_REGISTERED, SKSE_PAPYRUS_RUNNING
} SksePapyrusTrialPhase;

typedef struct {
    SksePapyrusTrialOps ops;
    void *context;
    uint32_t epoch, registry, active_owner;
    SksePapyrusTrialCallback callbacks[SKSE_PAPYRUS_TRIAL_MAX_OWNERS];
    size_t count;
    SksePapyrusTrialPhase phase;
} SksePapyrusTrial;

typedef struct {
    SksePapyrusTrialStatus status;
    size_t attempted;
    uint32_t owner; /* UINT32_MAX when no specific owner failed. */
} SksePapyrusTrialReport;

/* The table contains two little-endian guest words. The adapter supplies the
 * Register service address. An invalid request leaves the output unchanged. */
bool skse_papyrus_trial_interface(uint8_t *bytes, size_t size, uint32_t register_service);

/* Start one registration epoch in zero-initialized, caller-owned storage.
 * The caller serializes access and retains context and all mappings through
 * dispatch. This trial has no access to the live game registry. */
SksePapyrusTrialStatus skse_papyrus_trial_begin(SksePapyrusTrial *trial, uint32_t epoch,
                                               SksePapyrusTrialOps ops, void *context);

/* Retain one callback per owner. Repeating the same pair is idempotent. */
SksePapyrusTrialStatus skse_papyrus_trial_register(SksePapyrusTrial *trial, uint32_t owner,
                                                  uint32_t address);

/* An accepted mock registry seals the epoch before any callback runs. Check
 * all executable owners again and invoke in registration order. This trial
 * stops at the first failure. Success reports callback return only. */
SksePapyrusTrialReport skse_papyrus_trial_dispatch(SksePapyrusTrial *trial, uint32_t epoch,
                                                   uint32_t registry);

typedef struct {
    /* Read an aligned, little-endian word from that owner's isolated memory. */
    bool (*read_u32)(void *context, uint32_t owner, uint32_t address, uint32_t *out);
    /* Verify the frame, including its unaliased result and zero-argument state. */
    bool (*owns_frame)(void *context, uint32_t owner, uint32_t epoch, uint32_t stack_id,
                       uint32_t result, uint32_t state);
    /* Execute one interpreted x86 thiscall with the caller's work budget. */
    bool (*invoke_thiscall)(void *context, uint32_t owner, uint32_t address, uint32_t self,
                           const uint32_t *args, size_t count, uint32_t *eax);
} SksePapyrusTrialNativeOps;

typedef struct {
    const SksePapyrusTrial *trial;
    SksePapyrusTrialNativeOps ops;
    uint32_t epoch, owner, object, vtable, run;
} SksePapyrusTrialNative;

/* A native fixture can capture its checked static int() function during
 * the owner's RegisterFunctions callback. The caller verifies the signature
 * and owns the shadow registry and the initially zeroed host descriptor. No
 * guest pointer is treated as a host address. */
SksePapyrusTrialStatus skse_papyrus_trial_capture_int0(SksePapyrusTrial *trial, uint32_t registry,
                                                      uint32_t object, SksePapyrusTrialNativeOps ops,
                                                      SksePapyrusTrialNative *out);

/* A fixture provides fresh result/state scratch in its guest realm. The
 * result starts as None. A successful thiscall fills a type-3 VMValue and
 * returns 1 in AL. Failure leaves int_bits unchanged. */
SksePapyrusTrialStatus skse_papyrus_trial_run_int0(SksePapyrusTrial *trial,
                                                  const SksePapyrusTrialNative *native,
                                                  uint32_t stack_id, uint32_t result, uint32_t state,
                                                  uint32_t *int_bits);

#endif
