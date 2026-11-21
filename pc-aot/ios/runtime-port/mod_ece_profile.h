/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKYRIM_ECE_PROFILE_MATCH,
    SKYRIM_ECE_PROFILE_MISSING,
    SKYRIM_ECE_PROFILE_NONDEFAULT,
    SKYRIM_ECE_PROFILE_UNREADABLE
} SkyrimEceProfileStatus;

/* Compute SHA-256 of exactly length bytes of the borrowed fd, then check EOF.
 * The fd is read-only, regular, and positioned at byte zero. */
typedef bool (*SkyrimEceProfileDigest)(int fd, size_t length, uint8_t result[32]);

/* Check the loose winners in the already-deployed Data directory belonging to
 * the selected profile. The caller holds that view stable for the call. MATCH
 * identifies the reviewed default ECE 1.59 assets for the female Nord case.
 * The guest receiver and its selected FaceGen resource must be verified
 * independently before any application. */
SkyrimEceProfileStatus skyrim_ece_profile_check_female_nord(int data_fd, SkyrimEceProfileDigest digest);

#ifdef __cplusplus
}
#endif
