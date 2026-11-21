/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { SKYRIM_ECE_SLOT_LIMIT = 1024 * 1024 };

typedef enum {
    SKYRIM_ECE_SLOT_OK,
    SKYRIM_ECE_SLOT_TOO_LARGE,
    SKYRIM_ECE_SLOT_MALFORMED,
    SKYRIM_ECE_SLOT_NO_NOSE_HEIGHT,
    SKYRIM_ECE_SLOT_BAD_NOSE_HEIGHT
} SkyrimEceSlotStatus;

/* Read the raw signed NoseUpDown value (ID 12) of an ECE LE face slot.
 * The caller retains the source. The output is written only on success.
 * The target character's slider limits must be checked before applying. */
SkyrimEceSlotStatus skyrim_ece_slot_read_nose_height(const char *slot, size_t length, float *out);

#ifdef __cplusplus
}
#endif
