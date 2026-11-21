/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum SkyrimLifecycleSite { SkyrimSaveSlot = 1, SkyrimLoadSlot, SkyrimSaveVM, SkyrimLoadVM,
                          SkyrimRevertForLoad, SkyrimRevertForReload, SkyrimRegisterPapyrus };
enum SkyrimLifecycleResult { SkyrimNoResult, SkyrimReturnedFalse, SkyrimReturnedTrue };
enum { SkyrimLifecycleCapacity = 256 };

typedef struct { uint32_t site, ticket, parent; uint64_t generation; } SkyrimLifecycleCall;
typedef struct { uint32_t site, returning, outcome, ticket, parent; } SkyrimLifecycleRow;

typedef struct {
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    size_t count;
    uint64_t generation;
    uint32_t current;
    bool invalid;
} SkyrimLifecycleScope;

/* One save/load attempt per owner thread. Open before the first witnessed
 * operation, read the snapshot and close on the same thread, including on
 * cancellation. The scope must stay alive until close. */
bool skyrim_lifecycle_scope_open(SkyrimLifecycleScope *scope);
bool skyrim_lifecycle_scope_close(SkyrimLifecycleScope *scope);
/* Owner-thread identity for an active attempt. An idle scope has a
 * generation and no ticket. An invalid or absent scope returns zeroes. */
SkyrimLifecycleCall skyrim_lifecycle_scope_current(void);
SkyrimLifecycleCall skyrim_lifecycle_begin(uint32_t site);
void skyrim_lifecycle_end(SkyrimLifecycleCall call, uint32_t outcome);
/* On the owner thread, an open scope supplies only its own prefix. Otherwise
 * return the process-wide diagnostic prefix, ordered by reservation. A writer
 * still filling an earlier diagnostic row stops that prefix there. */
size_t skyrim_lifecycle_snapshot(SkyrimLifecycleRow *rows, size_t capacity, uint64_t *dropped);

#if defined(SKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE) && SKYRIM_EXPERIMENT_CLASSIC_LIFECYCLE
#define SKYRIM_LIFECYCLE_CALL(site, invocation, outcome) do { \
    SkyrimLifecycleCall skyrim_lifecycle_call = skyrim_lifecycle_begin(site); \
    invocation; \
    skyrim_lifecycle_end(skyrim_lifecycle_call, (outcome)); \
} while (0)
#else
#define SKYRIM_LIFECYCLE_CALL(site, invocation, outcome) invocation
#endif
