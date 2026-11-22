/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_highheel_scene_host.h"
#include "highheel_height.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

extern struct CPU *rt_current_cpu(void);
extern uint8_t *MEM;

enum {
    FIRST_MAPPED = 0x10000, NAME = 8, STOCK_NAMES = 0x12e2fc0,
    PLAYER_SINGLETON = 0x1b2e8e4, PLAYER_FORM = 0x14, FORM_ID = 0xc, FORM_TYPE = 0x12, CHARACTER = 0x3e,
    ACTOR_LOADED = 0x44, ACTOR_STATE = 0x68, LOADED_ROOT = 0x20, SWIMMING = 0x400, SMOOTH_STATE = 0x20,
    LOCAL_Z = 0x4c, LOCAL_SCALE = 0x50, WORLD_POSITION = 0x78, WORLD_SCALE = 0x84, POSE_MASK = 31,
    NODE_UPDATE = 0xaad9a0, FLAT_UPDATE = 0xabdf60, MAX_DEPTH = 32, MAX_VISITS = 128
};
static const uint32_t name_slots[] = {4, 0x1c, 0x20, 0x24, 0x28};

static _Thread_local struct {
    const struct CPU *owner;
    uint32_t root, path[MAX_DEPTH];
    uint32_t names[sizeof name_slots / sizeof *name_slots];
    ClassicHeelPose pose;
    unsigned depth, visits, kind, found, duplicate, captured;
    uint32_t before_node, before_z;
    unsigned frame;
    bool actor_checked, actor_current, swimming, smooth_this_state;
    bool frame_report, composition, same_z;
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

static bool capture_actor_state(uint32_t actor, uint32_t root) {
    if (root != scene.root || !node_address(actor) || actor > UINT32_MAX - ACTOR_STATE - 3 ||
        word(PLAYER_SINGLETON) != actor || word(actor + FORM_ID) != PLAYER_FORM ||
        MEM[actor + FORM_TYPE] != CHARACTER) return false;
    const uint32_t loaded = word(actor + ACTOR_LOADED);
    if (!node_address(loaded) || loaded > UINT32_MAX - LOADED_ROOT - 3 ||
        word(loaded + LOADED_ROOT) != root) return false;
    const uint32_t flags = word(actor + ACTOR_STATE);
    scene.swimming = (flags & SWIMMING) != 0;
    scene.smooth_this_state = (flags & SMOOTH_STATE) != 0;
    return true;
}

static unsigned update_kind(uint32_t target) {
    if (target == NODE_UPDATE) return 1;
    if (target == FLAT_UPDATE) return 2;
    return 0;
}

static float scalar(uint32_t address) {
    float value;
    memcpy(&value, MEM + address, sizeof value);
    return value;
}

static void capture_pose(unsigned index, uint32_t node) {
    if (node > UINT32_MAX - WORLD_SCALE - 3) return;
    if (!index) {
        scene.pose.local_scale = scalar(node + LOCAL_SCALE);
        scene.pose.world_scale = scalar(node + WORLD_SCALE);
    } else {
        ClassicHeelPoint *const points[] = {
            &scene.pose.left.foot, &scene.pose.right.foot, &scene.pose.left.calf, &scene.pose.right.calf,
        };
        const uint32_t position = node + WORLD_POSITION;
        *points[index - 1] = (ClassicHeelPoint){scalar(position), scalar(position + 4), scalar(position + 8)};
    }
    scene.captured |= 1u << index;
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
        scene.frame_report = third == SkyrimHighHeelSceneFrame && second <= SkyrimHighHeelFrameWaited;
        scene.frame = scene.frame_report ? second : SkyrimHighHeelFrameAbsent;
        return true;
    }
    if (scene.owner != cpu) return false;
    if (event == SkyrimHighHeelSceneActor) {
        if (scene.started || scene.actor_checked) return false;
        scene.actor_checked = true;
        scene.actor_current = capture_actor_state(first, second);
        return scene.actor_current;
    }
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
        const bool limited = scene.limited || first != scene.root || scene.depth != expected_depth;
        fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_SCENE schema=1 root=%u names=%u duplicate=%u limited=%u\n",
                scene.kind, scene.found, scene.duplicate, (unsigned)limited);
        if (scene.captured) {
            float unused_height;
            scene.pose.magnitude = 1.0f;
            const bool valid = !limited && !scene.duplicate && scene.captured == POSE_MASK &&
                               classic_heel_pose_height(&scene.pose, &unused_height);
            fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_POSE schema=1 captured=%u valid=%u\n",
                    scene.captured, (unsigned)valid);
        }
        if (scene.frame_report) {
            const bool composed = !limited && !(scene.duplicate & 1u) && scene.composition;
            fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_FRAME schema=1 phase=%u composed=%u same_z=%u\n",
                    scene.frame, (unsigned)composed, (unsigned)(composed && scene.same_z));
        }
        if (scene.actor_checked) {
            fprintf(stderr, "IOS_CLASSIC_HIGHHEEL_ACTOR schema=1 current=%u swimming=%u smooth_state=%u\n",
                    (unsigned)scene.actor_current, (unsigned)scene.swimming, (unsigned)scene.smooth_this_state);
        }
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
    case SkyrimHighHeelScenePreCompose:
        if (scene.frame_report && first == current && scene.depth > 1 &&
            second == scene.path[scene.depth - 2] && first <= UINT32_MAX - LOCAL_Z - 3 &&
            word(first + NAME) == scene.names[0]) {
            scene.before_node = first;
            scene.before_z = word(first + LOCAL_Z);
        }
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
            if (third == SkyrimHighHeelScenePose && !(scene.duplicate & bit)) {
                capture_pose((unsigned)i, first);
                if (!i && first == scene.before_node) {
                    scene.composition = true;
                    scene.same_z = scene.before_z == word(first + LOCAL_Z);
                    scene.before_node = 0;
                }
            }
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
