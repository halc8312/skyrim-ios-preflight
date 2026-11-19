/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winsync.h"

#define SYNC_TOKEN_FIRST UINT32_C(0xe4000000)
#define SYNC_TOKEN_LAST UINT32_C(0xe4ffffff)

enum { INVALID_HANDLE = 6, NO_MEMORY = 8, INVALID_PARAMETER = 87, TOO_MANY_POSTS = 298 };

static SkseWinWaitable *object_at(SkseWinSync *sync, uint32_t token) {
    if (!sync || !token) return NULL;
    for (unsigned i = 0; i < SKSE_WIN_SYNC_LIMIT; ++i)
        if (sync->objects[i].token == token) return &sync->objects[i];
    return NULL;
}

static uint32_t create(SkseWinSync *sync, SkseWinWaitable object) {
    if (!sync->next) sync->next = SYNC_TOKEN_FIRST;
    if (sync->next > SYNC_TOKEN_LAST) return 0;
    for (unsigned i = 0; i < SKSE_WIN_SYNC_LIMIT; ++i) {
        if (!sync->objects[i].token) {
            object.token = sync->next++;
            sync->objects[i] = object;
            return object.token;
        }
    }
    return 0;
}

uint32_t skse_win_event_create(SkseWinSync *sync, bool manual, bool signaled) {
    return create(sync, (SkseWinWaitable){.count = signaled, .maximum = 1,
                  .kind = manual ? SKSE_WIN_MANUAL_EVENT : SKSE_WIN_AUTO_EVENT});
}

bool skse_win_sync_close(SkseWinSync *sync, uint32_t token) {
    SkseWinWaitable *object = object_at(sync, token);
    if (!object) return false;
    *object = (SkseWinWaitable){0};
    return true;
}

bool skse_win_event_signal(SkseWinSync *sync, uint32_t token, bool signaled) {
    SkseWinWaitable *object = object_at(sync, token);
    if (!object || object->kind == SKSE_WIN_SEMAPHORE) return false;
    object->count = signaled;
    return true;
}

uint32_t skse_win_semaphore_create(SkseWinSync *sync, uint32_t initial, uint32_t maximum, uint32_t *error) {
    if (!maximum || maximum > INT32_MAX || initial > maximum) {
        *error = INVALID_PARAMETER;
        return 0;
    }
    uint32_t token = create(sync, (SkseWinWaitable){.count = initial, .maximum = maximum, .kind = SKSE_WIN_SEMAPHORE});
    if (!token) *error = NO_MEMORY;
    return token;
}

bool skse_win_semaphore_release(SkseWinSync *sync, uint32_t token, uint32_t count,
                                uint32_t *previous, uint32_t *error) {
    SkseWinWaitable *object = object_at(sync, token);
    if (!object || object->kind != SKSE_WIN_SEMAPHORE) {
        *error = INVALID_HANDLE;
        return false;
    }
    if (!count || count > INT32_MAX) {
        *error = INVALID_PARAMETER;
        return false;
    }
    if (count > object->maximum - object->count) {
        *error = TOO_MANY_POSTS;
        return false;
    }
    *previous = object->count;
    object->count += count;
    return true;
}

uint32_t skse_win_sync_wait(SkseWinSync *sync, const uint32_t *tokens, size_t count, bool all, uint32_t *error) {
    if (!count || count > SKSE_WIN_SYNC_LIMIT) {
        *error = INVALID_PARAMETER;
        return UINT32_MAX;
    }
    SkseWinWaitable *items[SKSE_WIN_SYNC_LIMIT];
    size_t selected = count, ready = 0;
    for (size_t i = 0; i < count; ++i) {
        items[i] = object_at(sync, tokens[i]);
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
        if (items[i]->count) {
            if (!ready) selected = i;
            ++ready;
        }
    }
    if (!ready || (all && ready < count)) return SKSE_WIN_WAIT_TIMEOUT;
    if (all) {
        for (size_t i = 0; i < count; ++i)
            if (items[i]->kind != SKSE_WIN_MANUAL_EVENT) --items[i]->count;
        return 0;
    }
    if (items[selected]->kind != SKSE_WIN_MANUAL_EVENT) --items[selected]->count;
    return (uint32_t)selected;
}
