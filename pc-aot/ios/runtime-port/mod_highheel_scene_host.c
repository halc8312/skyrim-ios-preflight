/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_highheel_scene_host.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

extern struct CPU *rt_current_cpu(void);
extern uint8_t *MEM;

enum {
    FIRST_MAPPED = 0x10000, NAME = 8, STOCK_NAMES = 0x12e2fc0,
    NODE_UPDATE = 0xaad9a0, FLAT_UPDATE = 0xabdf60, MAX_DEPTH = 32, MAX_VISITS = 128
};
static const uint32_t name_slots[] = {4, 0x1c, 0x20, 0x24, 0x28};

static _Thread_local struct {
    const struct CPU *owner;
    uint32_t root, path[MAX_DEPTH];
    uint32_t names[sizeof name_slots / sizeof *name_slots];
    unsigned depth, visits, kind, found, duplicate;
    bool started, limited;
} scene;

static uint32_t word(uint32_t address) {
    uint32_t value;
    memcpy(&value, MEM + address, sizeof value);
    return value;
}

static bool node_address(uint32_t value) {
    return value >= FIRST_MAPPED && !(value & 3) && value <= UINT32_MAX - NAME - 3;
}

static unsigned update_kind(uint32_t target) {
    if (target == NODE_UPDATE) return 1;
    if (target == FLAT_UPDATE) return 2;
    return 0;
}

static bool notify(const struct CPU *cpu, enum SkyrimHighHeelSceneNotice event, uint32_t first, uint32_t second,
                   uint32_t third) {
    if (!MEM || !cpu || cpu != rt_current_cpu()) return false;
    if (event == SkyrimHighHeelSceneArm) {
        if (scene.owner || !node_address(first)) return false;
        for (size_t i = 0; i < sizeof name_slots / sizeof *name_slots; ++i) {
            scene.names[i] = word(STOCK_NAMES + name_slots[i]);
            if (!scene.names[i]) return false;
        }
        scene.owner = cpu;
        scene.root = first;
        return true;
    }
    if (scene.owner != cpu) return false;
    if (event == SkyrimHighHeelSceneRoot) {
        if (scene.started) return false;
        scene.started = true;
        if (first == scene.root) scene.kind = update_kind(second);
        if (scene.kind) scene.path[scene.depth++] = first;
        scene.limited = !scene.kind;
        return true;  /* Still finish a recognized owner with an unknown update kind. */
    }
    if (!scene.started) return false;
    if (event == SkyrimHighHeelSceneFinish) {
        const unsigned expected_depth = scene.kind ? 1 : 0;
        const int saved_errno = errno;
        fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_SCENE schema=1 root=%u names=%u duplicate=%u limited=%u\n",
                scene.kind, scene.found, scene.duplicate,
                (unsigned)(scene.limited || first != scene.root || scene.depth != expected_depth));
        memset(&scene, 0, sizeof scene);
        errno = saved_errno;
        return true;
    }
    if (!scene.depth) return false;
    const uint32_t current = scene.path[scene.depth - 1];
    switch (event) {
    case SkyrimHighHeelSceneEnter:
        if (first != current) break;
        if (scene.depth == MAX_DEPTH || scene.visits >= MAX_VISITS || !node_address(second) || !update_kind(third)) {
            scene.limited = true;
            break;
        }
        ++scene.visits;
        scene.path[scene.depth++] = second;
        return true;
    case SkyrimHighHeelSceneLeave:
        if (scene.depth > 1 && second == current && first == scene.path[scene.depth - 2]) --scene.depth;
        else scene.limited = true;
        break;
    case SkyrimHighHeelSceneMember:
        if (first != current) break;
        if (scene.depth > 1 && second != scene.path[scene.depth - 2]) {
            scene.limited = true;
            break;
        }
        /* The stock node is reading its own fields here. No other guest
         * pointer is dereferenced or carried out of the original call. */
        const uint32_t name = word(first + NAME);
        for (size_t i = 0; i < sizeof name_slots / sizeof *name_slots; ++i) {
            if (name != scene.names[i]) continue;
            const unsigned bit = 1u << i;
            scene.duplicate |= scene.found & bit;
            scene.found |= bit;
            break;
        }
        break;
    default: break;
    }
    return false;
}

void skyrim_highheel_scene_fill(PcGuestModHooks *table) {
    table->highheel_scene.notify = notify;
}
