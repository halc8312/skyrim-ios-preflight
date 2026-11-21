/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct ClassicControlSubscription ClassicControlSubscription;

typedef enum {
    CLASSIC_REGISTER_CONTROL,
    CLASSIC_UNREGISTER_CONTROL,
    CLASSIC_UNREGISTER_ALL_CONTROLS
} ClassicControlOperation;

typedef struct {
    uint64_t handle;
    uint32_t action;  /* Guest interned BSFixedString data pointer. */
    const char *name;
    unsigned argc;
    float held_seconds;
} ClassicControlEvent;

typedef struct {
    void *context;
    void (*handle_ref)(void *context, uint64_t handle, bool retain);
    void (*action_ref)(void *context, uint32_t action, bool retain);
    /* Copy the event into the owning VM queue before returning. */
    void (*enqueue)(void *context, const ClassicControlEvent *event);
} ClassicControlVm;

typedef struct {
    ClassicControlVm vm;
    ClassicControlSubscription *head;
} ClassicControlBridge;

/* One owning VM. Serialize calls, including lifetime and enqueue callbacks.
 * The callbacks must not reenter the bridge or execute Papyrus handlers. */
bool classic_control_change(ClassicControlBridge *, ClassicControlOperation, uint64_t handle, uint32_t action);
void classic_control_edge(ClassicControlBridge *, uint32_t action, bool down, float held_seconds);
/* For deletion, remove subscriptions after the original guest sleep-unregister returns. */
void classic_control_unregister_all(ClassicControlBridge *, uint64_t handle);
/* The VM has already reset its queue and handle policy for a new world. */
void classic_control_after_vm_revert(ClassicControlBridge *);
/* Release subscriptions before detaching from a still-active VM. */
void classic_control_close(ClassicControlBridge *);
