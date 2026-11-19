/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum SkyrimLifecycleSite { SkyrimSaveSlot = 1, SkyrimLoadSlot, SkyrimSaveVM, SkyrimLoadVM,
                          SkyrimRevertForLoad, SkyrimRevertForReload, SkyrimRegisterPapyrus };
enum SkyrimLifecycleResult { SkyrimNoResult, SkyrimReturnedFalse, SkyrimReturnedTrue };
enum { SkyrimLifecycleCapacity = 256 };

typedef struct { uint32_t site, ticket, parent; } SkyrimLifecycleCall;
typedef struct { uint32_t site, returning, outcome, ticket, parent; } SkyrimLifecycleRow;

SkyrimLifecycleCall skyrim_lifecycle_begin(uint32_t site);
void skyrim_lifecycle_end(SkyrimLifecycleCall call, uint32_t outcome);
/* A process-local prefix ordered by reservation. A writer still filling an
 * earlier row stops this snapshot there. Re-read after writers are quiescent.
 * parent is the previous open ticket on this OS thread. A nonlocal guest exit
 * can leave an unmatched entry; its descendants require separate confirmation. */
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
