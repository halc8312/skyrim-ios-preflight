/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_FUZ_LIFT_VOICE_SEAM_H
#define SKYRIM_EXPERIMENT_FUZ_LIFT_VOICE_SEAM_H

#include "voice_fallback.h"

#include <stddef.h>
#include <stdint.h>

enum { SKYRIM_FUZ_LIFT_FRAME_BYTES = 0x104, SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT = 0x14,
       SKYRIM_FUZ_RESPONSE_TEXT_BYTES = 4096 };

typedef struct {
    // Each operation validates the full guest range before accessing it.
    bool (*read)(void *context, uint32_t address, void *bytes, size_t length);
    // Commit the whole replacement only if the range still matches expected.
    bool (*replace)(void *context, uint32_t address, const void *expected,
                    const void *replacement, size_t length);
    void *context;
} SkyrimFuzLiftMemory;

/* Copy a validated guest PE32 BSString, including its final NUL. */
bool skyrim_fuz_copy_response_text(const SkyrimFuzLiftMemory *, uint32_t response_object,
                                    char text[SKYRIM_FUZ_RESPONSE_TEXT_BYTES]);

/* Hosted adapter for the pinned Classic path assignment. The argument slot
 * holds the path pointer and the response object begins with a PE32 BSString.
 * The provider receives copied original path names and the authorized
 * stock names borrowed from policy. Guest pointers and response text stay
 * inside the adapter. The caller always performs the
 * original reference assignment after this optional stack-buffer update.
 * Subtitle routing is independent of this helper. */
bool skyrim_fuz_apply_lifted_path(const SkyrimFuzPolicy *policy, const SkyrimFuzLiftMemory *memory,
                                  uint32_t argument_slot, uint32_t response_object);

typedef struct {
    char assigned_path[SKYRIM_FUZ_LIFT_FRAME_BYTES];
    bool force_subtitle;
} SkyrimFuzLiftChoice;

/* Return a copied witness only when the response's stack path was replaced.
 * The original assignment still has to run before a caller uses the witness. */
bool skyrim_fuz_apply_lifted_path_choice(const SkyrimFuzPolicy *, const SkyrimFuzLiftMemory *,
                                         uint32_t argument_slot, uint32_t response_object, SkyrimFuzLiftChoice *);

/* Import supplied by the hosted fixture when a private lift is staged. */
void skyrim_fuz_host_before_voice_assign(uint32_t argument_slot, uint32_t response_object);
#endif
