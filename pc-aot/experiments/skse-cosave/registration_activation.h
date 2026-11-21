/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "event_bridge.h"
#include "registration_history.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t bytes[16]; } SkseVmEpoch;
typedef enum { SKSE_VM_UNAVAILABLE, SKSE_VM_CURRENT, SKSE_VM_RETIRED } SkseVmWorld;
typedef enum { SKSE_VM_RETAIN_ERROR, SKSE_VM_RETAINED, SKSE_VM_HANDLE_ABSENT } SkseVmRetain;
typedef struct {
    void *context;
    /* The authoritative VM owner supplies its recorded post-revert load.
     * It must have independently associated the loaded event with this exact
     * VM epoch. Do not copy back the arguments of the activation request. */
    int (*loaded)(void *, SkseVmEpoch *, SkseGameEvent *);
    /* RETIRED means the authoritative VM has already discarded this epoch
     * and all its references. A temporary failure must return UNAVAILABLE. */
    SkseVmWorld (*world)(void *, const SkseVmEpoch *);
    /* Borrow the identity of an existing name using the VM's own equality
     * rules. The same name must have one stable identity throughout an epoch. */
    int (*name_token)(void *, const SkseVmEpoch *, SkseCoreName, uint64_t *);
    /* The policy owner serializes VM access, epoch changes and these calls.
     * RETAIN_ERROR and HANDLE_ABSENT must acquire no reference. */
    SkseVmRetain (*retain)(void *, const SkseVmEpoch *, uint64_t handle);
    void (*release)(void *, const SkseVmEpoch *, uint64_t handle);
} SkseVmRegistrationPolicy;

typedef enum {
    SKSE_VM_REG_OK, SKSE_VM_REG_ARGUMENT, SKSE_VM_REG_BUSY, SKSE_VM_REG_OCCUPIED,
    SKSE_VM_REG_BRIDGE, SKSE_VM_REG_CORE, SKSE_VM_REG_FORMAT, SKSE_VM_REG_LIMIT,
    SKSE_VM_REG_MEMORY, SKSE_VM_REG_POLICY, SKSE_VM_REG_UNAVAILABLE, SKSE_VM_REG_RETIRED,
    SKSE_VM_REG_INACTIVE, SKSE_VM_REG_WRONG_LOAD
} SkseVmRegistrationResult;
typedef struct {
    SkseBridgeResult bridge;
    SksePairResult pair;
    SkseModMapResult map;
    SkseRegistrationResult format;
    uint32_t rows, missing_mod, duplicates, missing_handle, retained;
    /* Successful load's retained state, candidate allocation and name bytes.
     * Excludes the allocator's bookkeeping and VM-owned handle references. */
    uint32_t retained_bytes;
} SkseVmRegistrationReport;
typedef struct SkseVmRegistrations SkseVmRegistrations;
typedef struct SksePreparedRegistrations SksePreparedRegistrations;
typedef void (*SkseActiveRegistration)(void *, SkseRegistrationFamily, SkseRegistrationKey,
                                      SkseRegistrationName callback, uint64_t mapped_handle);

/* Each instance and all callbacks belong to one VM owner thread. The policy
 * and its context must remain callable until close succeeds. */
SkseVmRegistrationResult skse_vm_registrations_create(const SkseVmRegistrationPolicy *, SkseVmRegistrations **out);
/* Takes a verified scope, a complete successful load trace, independent proof
 * of the finalized .ess, and the policy's independent load/epoch identity.
 * No registration is visible until every participating handle is settled.
 * UNAVAILABLE after partial acquisition retains a private cleanup obligation. */
SkseVmRegistrationResult skse_bridge_load_registrations(SkseEventBridge *bridge, const SkseGameEvent *requested,
    const SkseEventTrace *trace, const SkseBaseEvidence *base, const SkseLoadedMod *current, size_t count,
    SkseVmRegistrations *registrations, SkseVmRegistrationReport *report);
/* Prepare from the exact immutable input consumed by the completed game load.
 * This consumes *pending on the bridge's owner thread and can hash and query
 * storage. Pass an initially empty *out. A successful preparation owns the
 * copied names, mapped handles and the
 * verified event. The preparation does not call the VM policy and may be
 * moved to the serialized VM owner after closing the bridge and input.
 * Use the current catalog from that same verified VM generation. */
SkseVmRegistrationResult skse_bridge_prepare_registrations(SkseLoadInput **pending,
    const SkseEventTrace *trace, const SkseBaseEvidence *base, const SkseLoadedMod *current, size_t count,
    SksePreparedRegistrations **out, SkseVmRegistrationReport *report);
/* Apply once on the VM policy's serialized owner thread. Calling with a
 * valid state and preparation consumes the preparation, including on failure.
 * The policy independently confirms the event and world before retaining any
 * handle. Do not hold the app epoch mutex here: policy calls may query it.
 * The VM dispatcher must serialize this invocation with native VM retire. */
SkseVmRegistrationResult skse_vm_registrations_apply(SkseVmRegistrations *, SksePreparedRegistrations **,
                                                     SkseVmRegistrationReport *report);
void skse_vm_registrations_drop(SksePreparedRegistrations **);
/* Enumerates a completed set in the current VM. Borrowed names expire on clear. */
SkseVmRegistrationResult skse_vm_registrations_visit(SkseVmRegistrations *, SkseActiveRegistration, void *context);
/* Call on cancellation, explicit unload, or after the authoritative VM revert.
 * Releases in reverse while the original epoch is live. After that epoch's VM
 * reset, discards bookkeeping without invoking its old handle policy.
 * UNAVAILABLE keeps pending references for retry and deactivates the set. */
SkseVmRegistrationResult skse_vm_registrations_clear(SkseVmRegistrations *);
/* Frees and clears *state on success. A temporarily inaccessible VM can
 * require a retry, leaving *state intact and no longer available to visit. */
SkseVmRegistrationResult skse_vm_registrations_close(SkseVmRegistrations **state);
#ifdef __cplusplus
}
#endif
