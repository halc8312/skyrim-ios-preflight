/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "classic_input_receiver.h"

/* The Classic event source supplies each control as an interned BSFixedString.
 * A named subscription owns one fixed string until the subscriber unregisters. */
typedef struct ClassicInputSubscription {
    struct ClassicInputSubscription *next;
    uint64_t handle;
    uint32_t registry, policy, key, owned_name;
} ClassicInputSubscription;

static ClassicInputSubscription *classic_input_subscriptions[2];
static unsigned classic_input_count[2];
static pthread_mutex_t classic_input_lock = PTHREAD_MUTEX_INITIALIZER;

static int skse_classic_input_subscribe(CPU *cpu, const char *class_name, unsigned op,
                                        uint32_t registry, uint32_t base, uint32_t key) {
    uint32_t policy;
    uint64_t handle;
    if (!skse_classic_input_target(cpu, class_name, registry, base, &policy, &handle)) return 0;
    int named = op >= S_REG_CONTROL && op <= S_UNREG_CONTROLS;
    int add = op == S_REG_CONTROL || op == S_REG_KEY;
    int all = op == S_UNREG_CONTROLS || op == S_UNREG_KEYS;

    pthread_mutex_lock(&classic_input_lock);
    ClassicInputSubscription **link = &classic_input_subscriptions[named];
    /* The SDK visits a subscription's recipients in ascending handle order. */
    while (*link && (*link)->handle < handle) link = &(*link)->next;
    while (*link && (*link)->handle == handle) {
        ClassicInputSubscription *entry = *link;
        if (entry->registry != registry || (!all && entry->key != key)) {
            link = &entry->next;
            continue;
        }
        if (add) {
            pthread_mutex_unlock(&classic_input_lock);
            return 1;
        }
        *link = entry->next;
        skse_classic_input_reference(cpu, entry->policy, handle, 0);
        if (entry->owned_name) {
            skse_member(cpu, 0xA511B0, entry->owned_name, 0, NULL);
            gfree(entry->owned_name);
        }
        free(entry);
        __atomic_sub_fetch(&classic_input_count[named], 1, __ATOMIC_RELEASE);
        if (!all) break;
    }
    if (add) {
        ClassicInputSubscription *entry = calloc(1, sizeof *entry);
        if (!entry) rt_fatal("SKSE input subscription allocation failed");
        entry->registry = registry;
        entry->policy = policy;
        entry->handle = handle;
        entry->key = key;
        if (named && key) {
            entry->owned_name = gmalloc(4);
            M32(entry->owned_name) = 0;
            skse_member(cpu, 0xA51210, entry->owned_name, 1, &key);
            entry->key = M32(entry->owned_name);
        }
        skse_classic_input_reference(cpu, policy, handle, 1);
        entry->next = *link;
        *link = entry;
        __atomic_add_fetch(&classic_input_count[named], 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&classic_input_lock);
    return 1;
}

typedef struct {
    uint64_t handle;
    uint32_t registry, policy;
} ClassicInputTarget;

/* Copy a single registration set, then unlock before touching the VM queue.
 * The SDK dispatches its key and control sets in two successive calls. */
static void skse_classic_dispatch_input(CPU *cpu, unsigned named, uint32_t key,
                                        int down, uint32_t timer_bits) {
    size_t total = 0;
    pthread_mutex_lock(&classic_input_lock);
    for (ClassicInputSubscription *s = classic_input_subscriptions[named]; s; s = s->next)
        if (s->key == key) ++total;
    if (!total) {
        pthread_mutex_unlock(&classic_input_lock);
        return;
    }
    ClassicInputTarget local[8];
    ClassicInputTarget *targets = local;
    if (total > sizeof local / sizeof local[0]) {
        if (total > SIZE_MAX / sizeof *targets) rt_fatal("SKSE input target count overflow");
        targets = malloc(total * sizeof *targets);
        if (!targets) rt_fatal("SKSE input target allocation failed");
    }
    size_t next = 0;
    for (ClassicInputSubscription *s = classic_input_subscriptions[named]; s; s = s->next) {
        if (s->key != key) continue;
        skse_classic_input_reference(cpu, s->policy, s->handle, 1);
        targets[next++] = (ClassicInputTarget){s->handle, s->registry, s->policy};
    }
    pthread_mutex_unlock(&classic_input_lock);

    uint32_t values = gmalloc(16);
    memset(GPTR(values), 0, 16);
    if (!down) {
        M32(values + 8) = 4;
        M32(values + 12) = timer_bits;
    }
    const char *event;
    if (named) {
        event = down ? "OnControlDown" : "OnControlUp";
        if (key) vm_string(cpu, values, str(key));
        else M32(values) = 2;
    } else {
        event = down ? "OnKeyDown" : "OnKeyUp";
        M32(values) = 3;
        M32(values + 4) = key;
    }
    for (size_t i = 0; i < total; ++i) {
        Registration recipient = {.registry = targets[i].registry, .handle = targets[i].handle};
        send_event(cpu, &recipient, event, down ? 1 : 2, values);
        skse_classic_input_reference(cpu, targets[i].policy, targets[i].handle, 0);
    }
    vm_clear(cpu, values);
    vm_clear(cpu, values + 8);
    gfree(values);
    if (targets != local) free(targets);
}

void skse_classic_button_event(CPU *cpu, uint32_t button, uint32_t key, int down) {
    uint32_t timer_bits = M32(button + 0x1c);
    if (__atomic_load_n(&classic_input_count[0], __ATOMIC_ACQUIRE))
        skse_classic_dispatch_input(cpu, 0, key, down, timer_bits);
    if (__atomic_load_n(&classic_input_count[1], __ATOMIC_ACQUIRE)) {
        /* The event owns the interned name for the lifetime of this call. */
        uint32_t control = skse_virtual(cpu, button, 2, 0, NULL);
        skse_classic_dispatch_input(cpu, 1, control ? M32(control) : 0, down, timer_bits);
    }
}
