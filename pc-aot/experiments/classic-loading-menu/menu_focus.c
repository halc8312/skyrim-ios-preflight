/* SPDX-License-Identifier: GPL-3.0-only */
#include "menu_focus.h"
#include "lift_prelude.h"

#include <string.h>

/* The Classic runtime's thiscall wrapper restores the entire caller CPU on return. */
extern uint32_t skse_member(CPU *, uint32_t address, uint32_t self, int argc, const uint32_t *args);
extern uint32_t gmalloc(uint32_t bytes);
extern void gfree(uint32_t address);

enum {
    INVENTORY_VTABLE = 0x10e5b90, MAGIC_VTABLE = 0x10e6594,
    INVENTORY_MESSAGE = 0x86ba80, MAGIC_MESSAGE = 0x875e40,
    CURSOR_SINGLETON = 0x1b2e974, STRING_HOLDER = 0x12e35e0,
    EXPIRY_NS = 250000000, MESSAGE_BYTES = 52
};

static bool span(uint32_t address, uint32_t size) {
    return address >= 0x10000u && address <= UINT32_MAX - (size - 1u);
}

static void invalidate(ClassicMenuFocus *state) {
    if (!++state->generation) ++state->generation;
    state->menu = state->view = 0;
    state->pending = false;
}

static void transition_locked(ClassicMenuFocus *state, ClassicMenu target, const CPU *owner) {
    invalidate(state);
    state->kind = (target == CLASSIC_MENU_INVENTORY || target == CLASSIC_MENU_MAGIC) ? target : CLASSIC_MENU_NONE;
    state->owner = owner;
    state->owner_thread = pthread_self();
}

void classic_menu_focus_transition(ClassicMenuFocus *state, ClassicMenu target, const CPU *owner) {
    pthread_mutex_lock(&state->lock);
    transition_locked(state, target, owner);
    pthread_mutex_unlock(&state->lock);
}

void classic_menu_focus_event(ClassicMenuFocus *state, ClassicMenuIdentity identity, bool opening, const CPU *owner) {
    if (!identity.name || identity.passive) return;
    pthread_mutex_lock(&state->lock);
    if (identity.kind == CLASSIC_MENU_INVENTORY || identity.kind == CLASSIC_MENU_MAGIC) {
        if (opening) state->lists |= identity.kind;
        else state->lists &= ~identity.kind;
    } else {
        unsigned i = 0;
        while (i < state->blocker_count && state->blockers[i] != identity.name) ++i;
        if (opening && i == state->blocker_count) {
            if (i == sizeof(state->blockers) / sizeof(state->blockers[0])) state->overflow = true;
            else state->blockers[state->blocker_count++] = identity.name;
        } else if (!opening && i < state->blocker_count) {
            state->blockers[i] = state->blockers[--state->blocker_count];
        }
    }
    transition_locked(state, !state->blocker_count && !state->overflow ? (ClassicMenu)state->lists : CLASSIC_MENU_NONE,
                      owner);
    pthread_mutex_unlock(&state->lock);
}

void classic_menu_focus_cancel(ClassicMenuFocus *state) {
    pthread_mutex_lock(&state->lock);
    invalidate(state);
    pthread_mutex_unlock(&state->lock);
}

uint64_t classic_menu_focus_token(ClassicMenuFocus *state) {
    pthread_mutex_lock(&state->lock);
    uint64_t token = state->view ? state->generation : 0;
    pthread_mutex_unlock(&state->lock);
    return token;
}

bool classic_menu_focus_post(ClassicMenuFocus *state, uint64_t token, double x, double y, uint64_t now_ns) {
    if (!token || !(x >= 0 && x < 1 && y >= 0 && y < 1)) return false;
    pthread_mutex_lock(&state->lock);
    bool accepted = token == state->generation && state->view;
    if (accepted) {
        state->x = x;
        state->y = y;
        state->submitted = now_ns;
        state->pending = true;
    }
    pthread_mutex_unlock(&state->lock);
    return accepted;
}

static bool send_hover(CPU *cpu, uint32_t menu, uint32_t view, ClassicMenu kind, double x, double y) {
    uint32_t cursor = M32(CURSOR_SINGLETON), holder = M32(STRING_HOLDER);
    if (!span(cursor, 0x1c) || !span(holder, 0x68) || M32(menu + 8) != view) return false;
    uint32_t name = M32(holder + (kind == CLASSIC_MENU_INVENTORY ? 0x44 : 0x64));
    float left = MF32(cursor + 0x0c), top = MF32(cursor + 0x10);
    float right = MF32(cursor + 0x14), bottom = MF32(cursor + 0x18);
    if (!name || !(left >= -32768 && top >= -32768 && right <= 32768 && bottom <= 32768 &&
                   left < right && top < bottom)) return false;
    uint32_t message = gmalloc(MESSAGE_BYTES);
    if (!span(message, MESSAGE_BYTES)) return false;
    uint32_t data = message + 16, event = data + 12;
    memset(MEM + message, 0, MESSAGE_BYTES);
    M32(message) = name;
    M32(message + 4) = 6;       /* UI Scaleform event */
    M32(message + 8) = data;
    M32(data + 8) = event;
    M32(event) = 1;             /* GFx mouse move with no buttons */
    MF32(event + 4) = (float)(left + x * (right - left));
    MF32(event + 8) = (float)(top + y * (bottom - top));
    uint32_t handler = kind == CLASSIC_MENU_INVENTORY ? INVENTORY_MESSAGE : MAGIC_MESSAGE;
    skse_member(cpu, handler, menu, 1, &message);
    gfree(message);
    return true;
}

bool classic_menu_focus_advance(ClassicMenuFocus *state, CPU *cpu, const CPU *thread_cpu,
                               uint32_t menu, uint32_t view, uint64_t now_ns) {
    if (!cpu || cpu != thread_cpu || !span(menu, 12) || !span(view, 4) || M32(menu + 8) != view) return false;
    uint32_t table = M32(menu);
    ClassicMenu kind;
    if (table == INVENTORY_VTABLE) kind = CLASSIC_MENU_INVENTORY;
    else if (table == MAGIC_VTABLE) kind = CLASSIC_MENU_MAGIC;
    else return false;
    double x = 0, y = 0;
    bool send = false;
    pthread_mutex_lock(&state->lock);
    if (state->kind == kind && state->owner == cpu && pthread_equal(state->owner_thread, pthread_self())) {
        if (state->menu != menu || state->view != view) {
            invalidate(state);
            state->menu = menu;
            state->view = view;
        } else if (state->pending) {
            send = now_ns >= state->submitted && now_ns - state->submitted <= EXPIRY_NS;
            x = state->x;
            y = state->y;
            state->pending = false;
        }
    }
    pthread_mutex_unlock(&state->lock);
    return send && send_hover(cpu, menu, view, kind, x, y);
}
