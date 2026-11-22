/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_lifetimes.h"

#include <stdlib.h>
#include <string.h>

uint64_t highheel_effect_handle(uint32_t actor, uint32_t number) {
    if (!actor || !number || number > UINT16_MAX) return 0;
    return ((uint64_t)(0x20000u | number) << 32) | actor;
}

static size_t find_slot(const HighHeelLifetimes *book, uint64_t handle) {
    uint32_t key = (uint32_t)handle ^ (uint32_t)(handle >> 32) * 0x9e3779b9u;
    key ^= key >> 16;
    for (size_t offset = 0; offset < book->capacity; ++offset) {
        const size_t index = (key + offset) & (book->capacity - 1);
        const uint64_t existing = book->slots[index].handle;
        if (!existing || existing == handle) return index;
    }
    return book->capacity;
}

static bool grow(HighHeelLifetimes *book) {
    if (book->capacity > SIZE_MAX / 2 / sizeof *book->slots) return false;
    const size_t capacity = book->capacity ? book->capacity * 2 : 16;
    HighHeelLifetime *slots = calloc(capacity, sizeof *slots);
    if (!slots) return false;
    HighHeelLifetimes expanded = *book;
    expanded.capacity = capacity;
    expanded.slots = slots;
    for (size_t i = 0; i < book->capacity; ++i) {
        const HighHeelLifetime row = book->slots[i];
        if (row.handle) slots[find_slot(&expanded, row.handle)] = row;
    }
    free(book->slots);
    *book = expanded;
    return true;
}

void highheel_lifetimes_observe(HighHeelLifetimes *book, uint64_t epoch, uint32_t registry,
                                const HighHeelEffectSample *sample) {
    if (!book || !sample || !epoch || !registry || epoch < book->epoch) return;
    const uint64_t handle = highheel_effect_handle(sample->actor_id, sample->number);
    if (!handle) return;
    if (epoch > book->epoch) {
        if (book->slots) memset(book->slots, 0, book->capacity * sizeof *book->slots);
        book->epoch = epoch;
        book->registry = registry;
        book->count = 0;
        book->incomplete = false;
    }
    if (book->registry != registry || book->incomplete) return;
    size_t index = find_slot(book, handle);
    if ((index == book->capacity || !book->slots[index].handle) && book->count >= book->capacity / 2) {
        if (!grow(book)) { book->incomplete = true; return; }
        index = find_slot(book, handle);
    }
    HighHeelLifetime *row = &book->slots[index];
    if (row->handle) {
        if (sample->starting) row->reused = true;
        else row->finished = true;
    } else {
        *row = (HighHeelLifetime){.handle = handle, .magnitude = sample->magnitude,
                                 .started = sample->starting, .finished = !sample->starting};
        ++book->count;
    }
}

bool highheel_lifetimes_match(const HighHeelLifetimes *book, uint64_t epoch, uint32_t registry,
                              uint64_t handle, uint32_t actor, bool keep, uint32_t magnitude) {
    if (!book || !handle || (uint32_t)handle != actor || !epoch || !registry ||
        book->epoch != epoch || book->registry != registry || book->incomplete) return false;
    const size_t index = find_slot(book, handle);
    if (index == book->capacity) return false;
    const HighHeelLifetime *row = &book->slots[index];
    return row->handle == handle && row->started && !row->reused &&
           (!keep || (!row->finished && row->magnitude == magnitude));
}

void highheel_lifetimes_clear(HighHeelLifetimes *book) {
    free(book->slots);
    *book = (HighHeelLifetimes){0};
}
