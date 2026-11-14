/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <pthread.h>

/* All real Metal commits enter here. No queue submission is discarded or
 * reported complete. At most 64 not-yet-scheduled buffers are retained; pressure
 * waits for scheduling, never for GPU completion. Normal commits do not wait. */
#define SKYRIM_GPU_GATE_SLOTS 64
typedef struct SkyrimGpuGateOps {
    void (*retain)(void *buffer);
    void (*release)(void *buffer);
    int (*scheduled)(void *buffer);
    void (*commit)(void *buffer);
    void (*wait_scheduled)(void *buffer);
} SkyrimGpuGateOps;
typedef struct SkyrimGpuGate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int active;
    unsigned committing;
    void *buffers[SKYRIM_GPU_GATE_SLOTS];
    unsigned char committed[SKYRIM_GPU_GATE_SLOTS];
} SkyrimGpuGate;
#define SKYRIM_GPU_GATE_INIT { .mutex=PTHREAD_MUTEX_INITIALIZER, \
    .changed=PTHREAD_COND_INITIALIZER, .active=1 }

/* pump is used only by the UIKit main thread while paused, outside our lock.
 * Worker callers pass NULL and sleep on the condition variable. */
void skyrim_gpu_gate_wait(SkyrimGpuGate *gate, void (*pump)(void));
/* Protect a whole native submission, including explicit enqueue before commit.
 * Nested commits remain admitted while pause drains the enclosing submission. */
void skyrim_gpu_gate_enter(SkyrimGpuGate *gate, void (*pump)(void));
void skyrim_gpu_gate_leave(SkyrimGpuGate *gate);
void skyrim_gpu_gate_commit(SkyrimGpuGate *gate, const SkyrimGpuGateOps *ops,
                            void *buffer, void (*pump)(void));
/* Main-thread lifecycle operations must be serialized. pause closes admission,
 * drains calls already inside commit, then waits for every tracked buffer to be
 * scheduled. It does not invoke commit, waitIdle, or waitUntilCompleted. */
void skyrim_gpu_gate_pause(SkyrimGpuGate *gate, const SkyrimGpuGateOps *ops);
void skyrim_gpu_gate_resume(SkyrimGpuGate *gate);

/* Application/platform integration, strong symbols required at link time. */
void skyrim_gpu_pause(void);
void skyrim_gpu_resume(void);
void skyrim_gpu_wait_active(void);
