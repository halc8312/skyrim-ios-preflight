/* SPDX-License-Identifier: GPL-3.0-only */
#include "lifecycle_controller.h"

/* One CAS publishes the count, admission and generation together. This closes
 * the enter/request race and rejects an old worker's completion after resume.
 * 20 bits leave ample room for concurrent host callers without wraparound. */
#define COUNT_MASK ((UINT64_C(1) << 20) - 1)
#define REQUESTED (UINT64_C(1) << 20)
#define CLOSED (UINT64_C(1) << 21)
#define DRAINING (UINT64_C(1) << 22)
#define QUIET (UINT64_C(1) << 23)
#define FAILED (UINT64_C(1) << 24)
#define GENERATION (UINT64_C(1) << 32)

static uint64_t read_word(const SkyrimLifecycleController *state) {
    return __atomic_load_n(&state->word, __ATOMIC_ACQUIRE);
}
static int replace_word(SkyrimLifecycleController *state, uint64_t *old, uint64_t next) {
    return __atomic_compare_exchange_n(&state->word, old, next, 1,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
void skyrim_lifecycle_controller_init(SkyrimLifecycleController *state) {
    __atomic_store_n(&state->word, 0, __ATOMIC_RELAXED);
}
int skyrim_lifecycle_controller_request(SkyrimLifecycleController *state, int active) {
    uint64_t old = read_word(state);
    for (;;) {
        uint64_t next = old;
        if (active) {
            next &= ~REQUESTED;
            /* An in-flight flush owns the closed interval until completion. */
            if (!(old & (DRAINING | FAILED))) next &= ~(CLOSED | QUIET);
        } else next |= REQUESTED | CLOSED;
        if (next == old) return 0;
        if (replace_word(state, &old, next)) return 1;
    }
}
int skyrim_lifecycle_controller_enter(SkyrimLifecycleController *state) {
    uint64_t old = read_word(state);
    for (;;) {
        if (old & FAILED) return -1;
        if (old & CLOSED) return 0;
        if ((old & COUNT_MASK) == COUNT_MASK) {
            if (replace_word(state, &old, old | FAILED | CLOSED)) return -1;
            continue;
        }
        if (replace_word(state, &old, old + 1)) return 1;
    }
}
int skyrim_lifecycle_controller_leave(SkyrimLifecycleController *state) {
    uint64_t old = read_word(state);
    for (;;) {
        if (!(old & COUNT_MASK)) {
            if (replace_word(state, &old, old | FAILED | CLOSED)) return -1;
            continue;
        }
        if (replace_word(state, &old, old - 1))
            return (old & COUNT_MASK) == 1 && (old & CLOSED) ? 1 : 0;
    }
}
uint32_t skyrim_lifecycle_controller_claim(SkyrimLifecycleController *state) {
    uint64_t old = read_word(state);
    for (;;) {
        if (!(old & REQUESTED) || !(old & CLOSED) ||
            (old & (COUNT_MASK | DRAINING | QUIET | FAILED))) return 0;
        if ((old >> 32) == UINT32_MAX) {
            if (replace_word(state, &old, old | FAILED)) return 0;
            continue;
        }
        uint64_t next = (old + GENERATION) | DRAINING;
        if (replace_word(state, &old, next)) return (uint32_t)(next >> 32);
    }
}
int skyrim_lifecycle_controller_complete(SkyrimLifecycleController *state,
                                       uint32_t generation, int32_t result) {
    uint64_t old = read_word(state);
    for (;;) {
        if (!generation || (uint32_t)(old >> 32) != generation || !(old & DRAINING)) return 0;
        uint64_t next = old & ~DRAINING;
        if (result) next |= FAILED | CLOSED;
        else if (old & REQUESTED) next |= QUIET | CLOSED;
        else next &= ~(CLOSED | QUIET);
        if (replace_word(state, &old, next)) return 1;
    }
}
SkyrimLifecycleSnapshot skyrim_lifecycle_controller_snapshot(const SkyrimLifecycleController *state) {
    const uint64_t word = read_word(state);
    SkyrimLifecycleSnapshot out = {(uint32_t)(word >> 32), SKYRIM_LIFECYCLE_ACTIVE,
        (uint32_t)(word & COUNT_MASK), !!(word & REQUESTED)};
    if (word & FAILED) out.phase = SKYRIM_LIFECYCLE_FAILED;
    else if (word & DRAINING) out.phase = SKYRIM_LIFECYCLE_DRAINING;
    else if (word & QUIET) out.phase = SKYRIM_LIFECYCLE_PAUSED;
    else if (word & CLOSED) out.phase = SKYRIM_LIFECYCLE_CLOSING;
    return out;
}
