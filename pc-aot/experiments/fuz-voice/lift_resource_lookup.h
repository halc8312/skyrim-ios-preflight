/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_FUZ_LIFT_RESOURCE_LOOKUP_H
#define SKYRIM_EXPERIMENT_FUZ_LIFT_RESOURCE_LOOKUP_H

#include "lift_voice_seam.h"

#include <stdint.h>

struct CPU;

/* Guest scratch below the caller's live stack: a gap, the 260-byte name
 * (rounded to 16), the stream object and the nested call frame. The game's
 * own resolver frames grow below that, as for an ordinary guest call. */
enum {
    SKYRIM_FUZ_GUEST_LOOKUP_GAP = 0x20, SKYRIM_FUZ_GUEST_LOOKUP_NAME = 0x110,
    SKYRIM_FUZ_GUEST_LOOKUP_OBJECT = 0x40, SKYRIM_FUZ_GUEST_LOOKUP_FRAME = 0x10,
    SKYRIM_FUZ_GUEST_LOOKUP_MIN_STACK = 0x10000
};

typedef struct {
    uint32_t name, object, frame;
} SkyrimFuzGuestLookupLayout;

typedef struct {
    const struct CPU *caller;  // Thread whose TLS base the lookup reuses.
    uint32_t live_stack;       // Lowest live guest stack address (caller esp).
} SkyrimFuzGuestResources;

bool skyrim_fuz_guest_lookup_layout(uint32_t live_stack, SkyrimFuzGuestLookupLayout *);

/* SkyrimFuzLookup through the reviewed Classic BSIStream constructor
 * f_B08630 and its destructor f_B08970 (flag 0, no heap release). An open
 * stream is PRESENT. No stream is ABSENT, as in LE 6.1, which cannot tell a
 * resolver failure from a missing name. A stream that fails to open is a
 * LOOKUP_ERROR, so the caller keeps the original path. */
SkyrimFuzAvailability skyrim_fuz_guest_resource_lookup(void *resources, const char *relative);

/* Run the configured policy with the game's resource lookup on the caller's
 * thread and update the stack path when a stock clip is selected. Existing
 * WAV, FUZ or XWM voices, a missing stock clip or any lookup failure leave
 * the original path unchanged. The caller still performs the original
 * assignment and owns the returned witness. */
bool skyrim_fuz_choose_with_guest_resources(const SkyrimFuzPolicy *configured, const SkyrimFuzLiftMemory *,
                                            const struct CPU *caller, uint32_t argument_slot,
                                            uint32_t response_object, SkyrimFuzLiftChoice *);
#endif
