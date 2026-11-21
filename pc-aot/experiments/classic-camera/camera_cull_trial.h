/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "camera_state.h"

enum {
    CLASSIC_CULL_PROBE_LIMIT = 128,
    CLASSIC_CULL_PRODUCER = 0,
    CLASSIC_CULL_FIRST = 1,
    CLASSIC_CULL_SECOND = 2,
    CLASSIC_CULL_STAGE_COUNT = 3
};

typedef struct {
    unsigned checked, clear, culled;
} ClassicCameraCullCounts;

typedef struct {
    unsigned stages;
    ClassicCameraCullCounts gates[CLASSIC_CULL_STAGE_COUNT];
} ClassicCameraCullTrialReport;

/* checked == CLASSIC_CULL_PROBE_LIMIT + 1 means that stage was truncated.
 * The output contains numbers only and never carries a guest address. */
typedef void (*ClassicCameraCullReport)(void *context, const ClassicCameraCullTrialReport *report);

bool classic_camera_cull_arm(const char *image_sha256, ClassicCameraReadWord read_word, void *guest,
                            ClassicCameraCullReport report, void *context);

/* Private adapter for the hosted experiment. The lift supplies identities from
 * the stock path. They are compared but never used to access an object. */
bool skyrim_camera_cull_prepare(uint32_t frame);
void skyrim_camera_cull_prepared(bool active);
void skyrim_camera_cull_finish(bool active);
void skyrim_camera_cull_producer(uint32_t caller, uint32_t node, bool clear);
bool skyrim_camera_cull_dispatch(uint32_t caller, uint32_t list, unsigned stage);
void skyrim_camera_cull_dispatched(bool active);
void skyrim_camera_cull_decision(uint32_t caller, uint32_t node, bool clear);

/* Shares the reviewed read-only camera layout with camera_state.c. The witness
 * is private and is discarded at the end of the original frame call. */
typedef struct {
    uint32_t camera, first_state, actor, loaded, body;
} ClassicCameraCullIdentity;

bool classic_camera_cull_identity(ClassicCameraReadWord read_word, void *guest, ClassicCameraCullIdentity *identity);
