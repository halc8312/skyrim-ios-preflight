/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "loading_menu.h"

struct CPU;

typedef struct {
    pthread_mutex_t lock;
    uint64_t generation, submitted;
    const struct CPU *owner;
    pthread_t owner_thread;
    ClassicMenu kind;
    uint32_t menu, view;
    uint32_t lists, blockers[64];
    unsigned blocker_count;
    double x, y;
    bool pending, overflow;
} ClassicMenuFocus;

#define CLASSIC_MENU_FOCUS_INIT { .lock = PTHREAD_MUTEX_INITIALIZER }

/* Call for each verified open/close transition. The CPU is borrowed only as an identity. */
void classic_menu_focus_event(ClassicMenuFocus *state, ClassicMenuIdentity identity, bool opening,
                             const struct CPU *owner);
void classic_menu_focus_cancel(ClassicMenuFocus *state);
uint64_t classic_menu_focus_token(ClassicMenuFocus *state);
bool classic_menu_focus_post(ClassicMenuFocus *state, uint64_t token, double x, double y, uint64_t now_ns);

/* Called synchronously while the engine advances this particular menu and movie.
 * The caller independently supplies the thread's current CPU. */
bool classic_menu_focus_advance(ClassicMenuFocus *state, struct CPU *cpu, const struct CPU *thread_cpu,
                               uint32_t menu, uint32_t view, uint64_t now_ns);
