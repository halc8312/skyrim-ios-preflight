/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_camera_host.h"
#include "camera_cull_trial.h"
#include "camera_state.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern uint8_t *MEM;

static char image_sha256[65];
static atomic_int last_sample = -1;

void skyrim_camera_app_configure(const char *verified_image_sha256) {
    if (!image_sha256[0] && classic_camera_image_supported(verified_image_sha256))
        memcpy(image_sha256, verified_image_sha256, sizeof image_sha256);
}

bool skyrim_camera_app_ready(void) { return image_sha256[0] != 0; }

static bool read_camera_word(void *memory, uint32_t address, uint32_t *value) {
    /* The guest reserves 4 GiB, with its first 64 KiB inaccessible. */
    if (!memory || address < 0x10000 || address > UINT32_MAX - (sizeof(*value) - 1)) return false;
    memcpy(value, (const uint8_t *)memory + address, sizeof(*value));
    return true;
}

void skyrim_camera_app_observe(uint32_t receiver) {
    ClassicCameraSample sample;
    if (!classic_camera_sample_after_return(image_sha256, CLASSIC_CAMERA_SET_STATE, receiver, read_camera_word,
                                            MEM, &sample))
        return;
    // Pack the slot and the two small classifications for log de-duplication.
    const int key = (sample.slot << 8) | (sample.arms << 4) | sample.body;
    const int previous = atomic_exchange_explicit(&last_sample, key, memory_order_relaxed);
    if (previous < 0 || (previous >> 8) != sample.slot)
        fprintf(stderr, "IOS_CAMERA_STATE slot=%d\n", sample.slot);
    if (sample.sampled_roots && previous != key)
        fprintf(stderr, "IOS_CAMERA_BODY schema=1 slot=%d arms=%d body=%d\n", sample.slot, sample.arms, sample.body);
}

void skyrim_camera_app_observe_render(uint32_t frame, uint32_t first, uint32_t second) {
    enum { SAMPLE_INTERVAL = 120, REPORT_LIMIT = 32 };
    static atomic_uint render_calls, reports;
    static atomic_int last_render = -1;
    if (!MEM || !skyrim_camera_app_ready() ||
        atomic_load_explicit(&reports, memory_order_relaxed) >= REPORT_LIMIT ||
        atomic_fetch_add_explicit(&render_calls, 1, memory_order_relaxed) % SAMPLE_INTERVAL)
        return;

    ClassicCameraRenderSample sample;
    if (!classic_camera_sample_render(image_sha256, frame, first, second, read_camera_word, MEM, &sample))
        return;
    const int key = (int)((sample.first << 4) | sample.second);
    if (atomic_exchange_explicit(&last_render, key, memory_order_relaxed) == key ||
        atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed) >= REPORT_LIMIT)
        return;
    const int saved_errno = errno;
    fprintf(stderr, "IOS_CAMERA_RENDER schema=1 first=%u second=%u\n", sample.first, sample.second);
    errno = saved_errno;
}

static atomic_bool cull_reported;
static _Thread_local bool cull_active;

static void report_cull(void *context, const ClassicCameraCullTrialReport *sample) {
    (void)context;
    if (atomic_exchange_explicit(&cull_reported, true, memory_order_relaxed)) return;
    const ClassicCameraCullCounts *producer = &sample->gates[CLASSIC_CULL_PRODUCER];
    const ClassicCameraCullCounts *first = &sample->gates[CLASSIC_CULL_FIRST];
    const ClassicCameraCullCounts *second = &sample->gates[CLASSIC_CULL_SECOND];
    const int saved_errno = errno;
    fprintf(stderr, "IOS_CAMERA_CULL schema=1 stages=%u producer=%u,%u,%u first=%u,%u,%u second=%u,%u,%u\n",
            sample->stages, producer->checked, producer->clear, producer->culled, first->checked, first->clear,
            first->culled, second->checked, second->clear, second->culled);
    errno = saved_errno;
}

bool skyrim_camera_app_observe_cull(enum SkyrimCameraCullNotice site, uint32_t first, uint32_t second, unsigned third) {
    if (site == SkyrimCameraCullPrepare) {
        enum { FRAME_INTERVAL = 120 };
        static _Thread_local unsigned frame_index;
        if (cull_active) return skyrim_camera_cull_prepare(first);  /* invalidate a nested frame */
        if (!MEM || !skyrim_camera_app_ready() || atomic_load_explicit(&cull_reported, memory_order_relaxed) ||
            frame_index++ % FRAME_INTERVAL ||
            !classic_camera_cull_arm(image_sha256, read_camera_word, MEM, report_cull, NULL)) return false;
        cull_active = skyrim_camera_cull_prepare(first);
        return cull_active;
    }
    if (!cull_active) return false;
    switch (site) {
    case SkyrimCameraCullPrepared: skyrim_camera_cull_prepared(true); break;
    case SkyrimCameraCullFinish: cull_active = false; skyrim_camera_cull_finish(true); break;
    case SkyrimCameraCullProducer: skyrim_camera_cull_producer(first, second, third != 0); break;
    case SkyrimCameraCullDispatch: return skyrim_camera_cull_dispatch(first, second, third);
    case SkyrimCameraCullDispatched: skyrim_camera_cull_dispatched(true); break;
    case SkyrimCameraCullDecision: skyrim_camera_cull_decision(first, second, third != 0); break;
    case SkyrimCameraCullPrepare: break;
    }
    return false;
}
