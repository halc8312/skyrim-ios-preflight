/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_IDLE_EVENT_H
#define CLASSIC_IDLE_EVENT_H

#include <stdbool.h>
#include <stdint.h>

enum { CLASSIC_IDLE_EVENT_BYTES = 128 };

typedef struct {
    uint32_t object_id;
    char event_name[CLASSIC_IDLE_EVENT_BYTES];
} ClassicIdleEvent;

typedef struct {
    bool present;
    uint32_t idle_id;
} ClassicIdleReplacement;

bool classic_idle_event_subscribed(void);
ClassicIdleReplacement classic_idle_event_choose(ClassicIdleEvent);

struct CPU;

/* The engine borrows an exclusively owned TESActionData request until end.
 * Applying a verified Idle uses a refcounted event-string assignment and sets
 * only the request's Idle pointer. A failed apply leaves both fields intact.
 * These services preserve guest CPU state and the original branch flag. */
bool classic_engine_idle_begin(const struct CPU *, uint32_t request);
bool classic_engine_idle_apply(const struct CPU *, uint32_t request, uint32_t idle_form);
void classic_engine_idle_end(const struct CPU *, uint32_t request);

void classic_idle_event_before_test(const struct CPU *, uint32_t request);

#endif
