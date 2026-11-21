/* SPDX-License-Identifier: GPL-3.0-only */
#include "control_bridge.h"

#include <stdlib.h>

struct ClassicControlSubscription {
    ClassicControlSubscription *next;
    uint64_t handle;
    uint32_t action;
};

static void remove_subscription(ClassicControlBridge *bridge, ClassicControlSubscription **link, bool release_handle) {
    ClassicControlSubscription *entry = *link;
    *link = entry->next;
    if (release_handle) bridge->vm.handle_ref(bridge->vm.context, entry->handle, false);
    bridge->vm.action_ref(bridge->vm.context, entry->action, false);
    free(entry);
}

bool classic_control_change(ClassicControlBridge *bridge, ClassicControlOperation operation, uint64_t handle, uint32_t action) {
    if (operation == CLASSIC_UNREGISTER_ALL_CONTROLS) {
        classic_control_unregister_all(bridge, handle);
        return true;
    }
    if (operation != CLASSIC_REGISTER_CONTROL && operation != CLASSIC_UNREGISTER_CONTROL) return false;
    if (!handle || !action) return true;

    /* Match the Classic set order within each interned control. */
    ClassicControlSubscription **link = &bridge->head;
    while (*link && ((*link)->action < action || ((*link)->action == action && (*link)->handle < handle)))
        link = &(*link)->next;
    bool present = *link && (*link)->action == action && (*link)->handle == handle;
    if (operation == CLASSIC_UNREGISTER_CONTROL) {
        if (present) remove_subscription(bridge, link, true);
        return true;
    }
    if (present) return true;

    ClassicControlSubscription *entry = malloc(sizeof(*entry));
    if (!entry) return false;
    *entry = (ClassicControlSubscription){.next = *link, .handle = handle, .action = action};
    bridge->vm.handle_ref(bridge->vm.context, handle, true);
    bridge->vm.action_ref(bridge->vm.context, action, true);
    *link = entry;
    return true;
}

void classic_control_edge(ClassicControlBridge *bridge, uint32_t action, bool down, float held_seconds) {
    if (!action) return;
    ClassicControlEvent event = {
        .action = action,
        .name = down ? "OnControlDown" : "OnControlUp",
        .argc = down ? 1u : 2u,
        .held_seconds = down ? 0.0f : held_seconds,
    };
    for (ClassicControlSubscription *entry = bridge->head; entry && entry->action <= action; entry = entry->next) {
        if (entry->action != action) continue;
        event.handle = entry->handle;
        bridge->vm.enqueue(bridge->vm.context, &event);
    }
}

void classic_control_unregister_all(ClassicControlBridge *bridge, uint64_t handle) {
    ClassicControlSubscription **link = &bridge->head;
    while (*link) {
        if ((*link)->handle == handle) remove_subscription(bridge, link, true);
        else link = &(*link)->next;
    }
}

void classic_control_after_vm_revert(ClassicControlBridge *bridge) {
    while (bridge->head) remove_subscription(bridge, &bridge->head, false);
}

void classic_control_close(ClassicControlBridge *bridge) {
    while (bridge->head) remove_subscription(bridge, &bridge->head, true);
}
