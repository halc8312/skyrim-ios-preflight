/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_cull_trial.h"

enum { PHASE_NONE = -1, FRAME_FIRST = 0xC8, FRAME_SECOND = 0xD4 };

typedef struct {
    bool armed, active, invalid;
    int phase;
    uint32_t frame;
    ClassicCameraReadWord read_word;
    void *guest;
    ClassicCameraCullReport report;
    void *context;
    ClassicCameraCullIdentity identity;
    ClassicCameraCullTrialReport output;
} CameraCullTrial;

static _Thread_local CameraCullTrial trial;

bool classic_camera_cull_arm(const char *image_sha256, ClassicCameraReadWord read_word, void *guest,
                            ClassicCameraCullReport report, void *context) {
    if (trial.armed || trial.active || !read_word || !report || !classic_camera_image_supported(image_sha256))
        return false;
    trial = (CameraCullTrial){.armed = true, .phase = PHASE_NONE, .read_word = read_word, .guest = guest,
                              .report = report, .context = context};
    return true;
}

bool skyrim_camera_cull_prepare(uint32_t frame) {
    if (trial.active) {
        trial.invalid = true;
        return false;
    }
    if (!trial.armed) return false;
    trial.armed = false;
    if (frame < 0x10000 || frame % 4 || frame > UINT32_MAX - FRAME_SECOND ||
        !classic_camera_cull_identity(trial.read_word, trial.guest, &trial.identity)) {
        trial = (CameraCullTrial){0};
        return false;
    }
    trial.active = true;
    trial.frame = frame;
    trial.phase = CLASSIC_CULL_PRODUCER;
    trial.output.stages = 1u << CLASSIC_CULL_PRODUCER;
    return true;
}

void skyrim_camera_cull_prepared(bool active) {
    if (active) trial.phase = PHASE_NONE;
}

static bool same_identity(void) {
    ClassicCameraCullIdentity current;
    return classic_camera_cull_identity(trial.read_word, trial.guest, &current) &&
           current.camera == trial.identity.camera && current.first_state == trial.identity.first_state &&
           current.actor == trial.identity.actor && current.loaded == trial.identity.loaded &&
           current.body == trial.identity.body;
}

void skyrim_camera_cull_finish(bool active) {
    if (!active) return;
    const bool report = !trial.invalid && trial.phase == PHASE_NONE && same_identity();
    const ClassicCameraCullTrialReport output = trial.output;
    ClassicCameraCullReport emit = trial.report;
    void *context = trial.context;
    trial = (CameraCullTrial){0};
    if (report) emit(context, &output);
}

static void count_gate(uint32_t node, bool clear) {
    ClassicCameraCullCounts *counts = &trial.output.gates[trial.phase];
    if (counts->checked > CLASSIC_CULL_PROBE_LIMIT) return;
    if (++counts->checked > CLASSIC_CULL_PROBE_LIMIT || node != trial.identity.body) return;
    if (clear) ++counts->clear;
    else ++counts->culled;
}

void skyrim_camera_cull_producer(uint32_t caller, uint32_t node, bool clear) {
    // f_698800's original call to f_694FD0.
    if (trial.active && !trial.invalid && trial.phase == CLASSIC_CULL_PRODUCER && caller == 0x698866)
        count_gate(node, clear);
}

bool skyrim_camera_cull_dispatch(uint32_t caller, uint32_t list, unsigned stage) {
    // Only the two retained lists of the one observed frame at f_69A9F0's
    // original call to f_CB9E20.
    if (!trial.active || trial.invalid || caller != 0x69AB11 ||
        (stage != CLASSIC_CULL_FIRST && stage != CLASSIC_CULL_SECOND) ||
        list != trial.frame + (stage == CLASSIC_CULL_FIRST ? FRAME_FIRST : FRAME_SECOND))
        return false;
    if (trial.phase != PHASE_NONE || (trial.output.stages & (1u << stage))) {
        trial.invalid = true;
        return false;
    }
    trial.phase = (int)stage;
    trial.output.stages |= 1u << stage;
    return true;
}

void skyrim_camera_cull_dispatched(bool active) {
    if (active) trial.phase = PHASE_NONE;
}

void skyrim_camera_cull_decision(uint32_t caller, uint32_t node, bool clear) {
    // The original f_AAF430 bit test when called by f_B07E70 or NiNode traversal.
    if (trial.active && !trial.invalid && trial.phase >= CLASSIC_CULL_FIRST &&
        (caller == 0xB07F04 || caller == 0xAAE173))
        count_gate(node, clear);
}
