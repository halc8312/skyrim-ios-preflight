/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SkyrimLockCategory {
    SKYRIM_LOCK_D3D9 = 1, SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_COM,
    SKYRIM_LOCK_SHADOW, SKYRIM_LOCK_BC
};
enum SkyrimLockPhase {
    SKYRIM_LOCK_WAIT = 1, SKYRIM_LOCK_HOLD, SKYRIM_LOCK_WORK
};
enum SkyrimLockOperation {
    SKYRIM_LOCK_OP_UNKNOWN,
    SKYRIM_LOCK_OP_D3D9_OTHER, SKYRIM_LOCK_OP_D3D9_PRESENT,
    SKYRIM_LOCK_OP_AUDIO_OTHER, SKYRIM_LOCK_OP_AUDIO_SUBMIT,
    SKYRIM_LOCK_OP_AUDIO_GET_STATE, SKYRIM_LOCK_OP_AUDIO_FLUSH,
    SKYRIM_LOCK_OP_AUDIO_MIX, SKYRIM_LOCK_OP_AUDIO_DESTROY,
    SKYRIM_LOCK_OP_COM_LOOKUP, SKYRIM_LOCK_OP_COM_WRAP, SKYRIM_LOCK_OP_COM_FORGET,
    SKYRIM_LOCK_OP_SHADOW_CLEANUP, SKYRIM_LOCK_OP_BC_CLEANUP,
    SKYRIM_LOCK_OP_ADPCM_DECODE, SKYRIM_LOCK_OP_AUDIO_RESET
};

/* All identities are process-local ordinals, never pointers, OS thread IDs,
 * guest handles or game data. Zero means unavailable. Owner fields describe
 * one observation, NOT every owner encountered or a proven dependency chain.
 * cpu_ns is the measured current-thread CPU interval, not GPU/owner CPU time.
 * Hold is outermost-only; work may be nested and must not be added to hold. */
typedef struct SkyrimLockEvent {
    uint64_t start_ns, end_ns, instance, thread, owner_thread, cpu_ns;
    unsigned lock, phase, operation, owner_operation, cpu_valid;
} SkyrimLockEvent;

#define SKYRIM_LOCK_THRESHOLD_NS UINT64_C(16666667)
#define SKYRIM_LOCK_CAPACITY 16
typedef struct SkyrimLockWindow {
    /* Retain the longest completed intervals in this drained window, not a
     * complete trace. Equal-duration newcomers do not displace older samples.
     * A full-buffer arrival always counts one discarded event (old or new).
     * One busy category can still crowd out another category's shorter waits. */
    SkyrimLockEvent events[SKYRIM_LOCK_CAPACITY];
    unsigned count;
    uint64_t dropped_busy, dropped_full;
    /* phase 1..3 => slot 0..2; approximate windows, observed intervals only.
     * Not a count of all calls/locks: untimed fast paths are deliberately absent.
     * These wall-time sums overlap across threads/phases and are NOT frame cost. */
    uint64_t observed_count[3], observed_ns[3];
} SkyrimLockWindow;

/* Writers never wait, allocate, read clocks, perform I/O or touch guest memory.
 * Invalid intervals and intervals spanning inactivity are ignored. Optional
 * CPU values outside the enclosing wall interval become unavailable. */
void skyrim_lock_record(const SkyrimLockEvent *event);
/* Single reader. Returns zero without draining when a writer owns the buffer. */
int skyrim_lock_take(SkyrimLockWindow *out);
/* Called only by the existing five-second UI reporter, not lock wrappers. */
void skyrim_lock_log_window(void);
/* Zero means unavailable. Reads current-thread CPU time, preserving errno.
 * Use only around real contention; do not add this to normal lock fast paths. */
uint64_t skyrim_lock_cpu_now_ns(void);

#ifdef __cplusplus
}
#endif
