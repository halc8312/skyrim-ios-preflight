/* SPDX-License-Identifier: GPL-3.0-only */
#include "gpu_gate.h"
#include <assert.h>
#include <stddef.h>

static _Thread_local SkyrimGpuGate *inside_gate;
static _Thread_local unsigned inside_depth;

static void wait_active(SkyrimGpuGate *g, void (*pump)(void)) {
    while (!g->active && !(inside_gate == g && inside_depth)) {
        if (pump) {
            pthread_mutex_unlock(&g->mutex);
            pump();
            pthread_mutex_lock(&g->mutex);
        } else {
            pthread_cond_wait(&g->changed, &g->mutex);
        }
    }
}

void skyrim_gpu_gate_wait(SkyrimGpuGate *g, void (*pump)(void)) {
    pthread_mutex_lock(&g->mutex);
    wait_active(g, pump);
    pthread_mutex_unlock(&g->mutex);
}

void skyrim_gpu_gate_enter(SkyrimGpuGate *g, void (*pump)(void)) {
    assert(!inside_depth || inside_gate == g);
    pthread_mutex_lock(&g->mutex);
    wait_active(g, pump);
    ++g->committing;
    inside_gate = g;
    ++inside_depth;
    pthread_mutex_unlock(&g->mutex);
}

void skyrim_gpu_gate_leave(SkyrimGpuGate *g) {
    assert(inside_gate == g && inside_depth);
    pthread_mutex_lock(&g->mutex);
    --inside_depth;
    if (!inside_depth) inside_gate = NULL;
    --g->committing;
    pthread_cond_broadcast(&g->changed);
    pthread_mutex_unlock(&g->mutex);
}

void skyrim_gpu_gate_commit(SkyrimGpuGate *g, const SkyrimGpuGateOps *ops,
                            void *buffer, void (*pump)(void)) {
    if (!buffer) return; /* Preserve Objective-C's original nil commit. */
    skyrim_gpu_gate_enter(g, pump);
    unsigned slot;
    void *retired[SKYRIM_GPU_GATE_SLOTS];
    unsigned retired_count = 0;
    pthread_mutex_lock(&g->mutex);
    for (;;) {
        slot = SKYRIM_GPU_GATE_SLOTS;
        void *waiter = NULL;
        for (unsigned i = 0; i < SKYRIM_GPU_GATE_SLOTS; ++i) {
            if (g->buffers[i] && g->committed[i] && ops->scheduled(g->buffers[i])) {
                retired[retired_count++] = g->buffers[i];
                g->buffers[i] = NULL;
                g->committed[i] = 0;
            }
            if (!g->buffers[i] && slot == SKYRIM_GPU_GATE_SLOTS) slot = i;
            if (g->buffers[i] && g->committed[i] && !waiter) waiter = g->buffers[i];
        }
        if (slot < SKYRIM_GPU_GATE_SLOTS) break;
        if (inside_depth > 1) {
            /* An enclosing submission may have enqueued this buffer before
             * the older committed buffers. Commit it before waiting, otherwise
             * those buffers can never be scheduled. No slot is needed once
             * this call has observed scheduling itself. */
            pthread_mutex_unlock(&g->mutex);
            ops->commit(buffer);
            ops->wait_scheduled(buffer);
            skyrim_gpu_gate_leave(g);
            return;
        }
        if (waiter) {
            /* Keep ownership even if pause drains this slot concurrently. */
            ops->retain(waiter);
            pthread_mutex_unlock(&g->mutex);
            ops->wait_scheduled(waiter);
            ops->release(waiter);
            pthread_mutex_lock(&g->mutex);
        } else {
            /* Other calls reserved every slot but have not returned from
             * commit yet. Their retirement/close always signals this cond. */
            pthread_cond_wait(&g->changed, &g->mutex);
        }
    }
    ops->retain(buffer);
    g->buffers[slot] = buffer;
    g->committed[slot] = 0;
    pthread_mutex_unlock(&g->mutex);
    /* Retire every scheduled entry, not only the first reusable slot: otherwise
     * an old burst's upper slots would keep Metal resources alive indefinitely. */
    for (unsigned i = 0; i < retired_count; ++i) ops->release(retired[i]);

    /* No gate lock across Metal or completion callbacks. A synchronous nested
     * commit is part of this admitted operation and is drained by pause too. */
    ops->commit(buffer);

    pthread_mutex_lock(&g->mutex);
    g->committed[slot] = 1;
    pthread_cond_broadcast(&g->changed);
    pthread_mutex_unlock(&g->mutex);
    skyrim_gpu_gate_leave(g);
}

void skyrim_gpu_gate_pause(SkyrimGpuGate *g, const SkyrimGpuGateOps *ops) {
    /* UIKit must not invoke lifecycle callbacks from inside Metal's commit. */
    assert(inside_gate != g || !inside_depth);
    void *pending[SKYRIM_GPU_GATE_SLOTS];
    unsigned count = 0;
    pthread_mutex_lock(&g->mutex);
    g->active = 0;
    while (g->committing) pthread_cond_wait(&g->changed, &g->mutex);
    for (unsigned i = 0; i < SKYRIM_GPU_GATE_SLOTS; ++i) {
        if (g->buffers[i]) pending[count++] = g->buffers[i];
        g->buffers[i] = NULL;
        g->committed[i] = 0;
    }
    pthread_cond_broadcast(&g->changed);
    pthread_mutex_unlock(&g->mutex);
    for (unsigned i = 0; i < count; ++i) {
        if (!ops->scheduled(pending[i])) ops->wait_scheduled(pending[i]);
        ops->release(pending[i]);
    }
}

void skyrim_gpu_gate_resume(SkyrimGpuGate *g) {
    pthread_mutex_lock(&g->mutex);
    g->active = 1;
    pthread_cond_broadcast(&g->changed);
    pthread_mutex_unlock(&g->mutex);
}
