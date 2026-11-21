/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "lift_prelude.h"
#include "mod_event.h"

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

typedef struct {
    uint64_t handle;
    uint32_t registry;
    uint64_t epoch;
    const char *event, *callback;
} FixtureSubscription;

void classic_fixture_subscribe(const FixtureSubscription *, size_t count);
void classic_fixture_send_typed(CPU *, uint32_t registry, uint64_t epoch, const ClassicModEventPacket *);
void skse_send_mod_event(CPU *, const char *name, const char *text, float number, uint32_t sender);
