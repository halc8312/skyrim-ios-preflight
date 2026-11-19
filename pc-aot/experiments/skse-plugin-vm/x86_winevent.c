/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winevent.h"

#define EVENT_TOKEN_FIRST UINT32_C(0xe4000000)
#define EVENT_TOKEN_LAST UINT32_C(0xe4ffffff)

static SkseWinEvent *event_at(SkseWinEvents *events, uint32_t token) {
    if (!events || !token) return NULL;
    for (unsigned i = 0; i < SKSE_WIN_EVENT_LIMIT; ++i)
        if (events->objects[i].token == token) return &events->objects[i];
    return NULL;
}

uint32_t skse_win_event_create(SkseWinEvents *events, bool manual, bool signaled) {
    if (!events->next) events->next = EVENT_TOKEN_FIRST;
    if (events->next > EVENT_TOKEN_LAST) return 0;
    for (unsigned i = 0; i < SKSE_WIN_EVENT_LIMIT; ++i) {
        SkseWinEvent *event = &events->objects[i];
        if (!event->token) {
            const uint32_t token = events->next++;
            *event = (SkseWinEvent){.token = token, .manual = manual, .signaled = signaled};
            return token;
        }
    }
    return 0;
}

bool skse_win_event_close(SkseWinEvents *events, uint32_t token) {
    SkseWinEvent *event = event_at(events, token);
    if (!event) return false;
    *event = (SkseWinEvent){0};
    return true;
}

bool skse_win_event_signal(SkseWinEvents *events, uint32_t token, bool signaled) {
    SkseWinEvent *event = event_at(events, token);
    if (!event) return false;
    event->signaled = signaled;
    return true;
}

uint32_t skse_win_event_wait(SkseWinEvents *events, const uint32_t *tokens, size_t count, bool all, uint32_t *error) {
    enum { INVALID_HANDLE = 6, INVALID_PARAMETER = 87 };
    if (!count || count > SKSE_WIN_EVENT_LIMIT) {
        *error = INVALID_PARAMETER;
        return UINT32_MAX;
    }
    SkseWinEvent *items[SKSE_WIN_EVENT_LIMIT];
    size_t selected = count, ready = 0;
    for (size_t i = 0; i < count; ++i) {
        items[i] = event_at(events, tokens[i]);
        if (!items[i]) {
            *error = INVALID_HANDLE;
            return UINT32_MAX;
        }
        for (size_t j = 0; j < i; ++j) {
            if (items[j] == items[i]) {
                *error = INVALID_PARAMETER;
                return UINT32_MAX;
            }
        }
        if (items[i]->signaled) {
            if (!ready) selected = i;
            ++ready;
        }
    }
    if (!ready || (all && ready < count)) return SKSE_WIN_WAIT_TIMEOUT;
    if (all) {
        for (size_t i = 0; i < count; ++i)
            if (!items[i]->manual) items[i]->signaled = false;
        return 0;
    }
    if (!items[selected]->manual) items[selected]->signaled = false;
    return (uint32_t)selected;
}
