/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "lift_voice_seam.h"

enum { SKYRIM_FUZ_MAX_MARKED_RESPONSES = 64 };

typedef struct {
    uint32_t object, voice_reference;
    uint64_t serial;
} SkyrimFuzResponseMark;

typedef struct {
    SkyrimFuzResponseMark active[SKYRIM_FUZ_MAX_MARKED_RESPONSES];
    size_t count;
    uint64_t issued;
} SkyrimFuzMenuTracker;

typedef struct {
    uint32_t manager, node, selection, response, title, voice_reference;
    uint64_t serial;
} SkyrimFuzMenuScope;

/* Call on reviewed construction and destruction boundaries, with one
 * serialized tracker for the guest world's lifetime. */
void skyrim_fuz_menu_forget(SkyrimFuzMenuTracker *, uint32_t response);
bool skyrim_fuz_menu_assigned(SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                              uint32_t response, const SkyrimFuzLiftChoice *);

/* Check the live accepted assignment, original voice argument and text
 * pointer together. A return of zero carries no substitution. */
uint64_t skyrim_fuz_substitution_serial(const SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                                       uint32_t response, uint32_t voice_argument, uint32_t title_argument);

/* Owned numeric evidence for a single, synchronous selected-response call. */
SkyrimFuzMenuScope skyrim_fuz_menu_capture(const SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                                           uint32_t manager, uint32_t node, uint32_t queued_title);
bool skyrim_fuz_menu_force(const SkyrimFuzMenuTracker *, const SkyrimFuzLiftMemory *,
                           const SkyrimFuzMenuScope *, uint32_t return_address, uint32_t queued_title);

/* Host-side imports used only by the privately staged lift. */
struct CPU;
void skyrim_fuz_host_forget_response(uint32_t response);
void skyrim_fuz_host_choose_menu_voice(uint32_t argument_slot, uint32_t response, SkyrimFuzLiftChoice *);
void skyrim_fuz_host_note_menu_assignment(uint32_t response, const SkyrimFuzLiftChoice *);
SkyrimFuzMenuScope skyrim_fuz_host_capture_menu(uint32_t manager, uint32_t node, uint32_t queued_title);
bool skyrim_fuz_host_force_menu(const SkyrimFuzMenuScope *, uint32_t return_address, uint32_t queued_title);
void skyrim_fuz_host_queue_selected(struct CPU *, const SkyrimFuzMenuScope *);
