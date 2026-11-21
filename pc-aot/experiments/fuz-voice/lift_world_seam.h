/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "lift_menu_seam.h"

enum { SKYRIM_FUZ_WORLD_RECORD_BYTES = 0x14, SKYRIM_FUZ_MAX_CHATTER_MARKS = 64 };

typedef struct {
    uint32_t response, submitted_voice, queued_title;
    uint64_t serial;
} SkyrimFuzWorldScope;

typedef struct {
    uint32_t array, owned_title, actor_handle;
    uint16_t length, capacity;
} SkyrimFuzChatterMark;

typedef struct {
    SkyrimFuzChatterMark active[SKYRIM_FUZ_MAX_CHATTER_MARKS];
    size_t count;
} SkyrimFuzWorldTracker;

/* One serialized guest world. The published identity belongs only to the
 * original chatter queue and is retired before the owned title is freed. */
SkyrimFuzWorldScope skyrim_fuz_world_capture(const SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                                            uint32_t response, uint32_t submitted_voice);
bool skyrim_fuz_world_admit(const SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                            const SkyrimFuzWorldScope *, uint32_t return_address, uint32_t title);
void skyrim_fuz_world_after_copy(SkyrimFuzWorldTracker *, const SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                                uint32_t array, uint32_t index, uint32_t source, const SkyrimFuzWorldScope *);
void skyrim_fuz_world_release_title(SkyrimFuzWorldTracker *, uint32_t title);
/* Invoke only after the original spatial and duplicate-speaker checks. */
bool skyrim_fuz_world_general(const SkyrimFuzWorldTracker *, const SkyrimFuzLiftMemory *, uint32_t array, uint32_t record);

/* Host imports for the pinned, privately staged experiment. */
SkyrimFuzWorldScope skyrim_fuz_host_capture_world(uint32_t response, uint32_t voice);
bool skyrim_fuz_host_admit_world(const SkyrimFuzWorldScope *, uint32_t return_address, uint32_t title);
void skyrim_fuz_host_world_after_copy(uint32_t array, uint32_t index, uint32_t source, const SkyrimFuzWorldScope *);
void skyrim_fuz_host_release_chatter_title(uint32_t title);
bool skyrim_fuz_host_world_general(uint32_t array, uint32_t record);
void skyrim_fuz_host_queue_world(struct CPU *, const SkyrimFuzWorldScope *);
