/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "lift_prelude.h"
#ifdef SKYRIM_TEST_SCENE_RUNTIME
#include "mod_highheel_scene_host.h"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TLS = 0x11000, TLS_TABLE = 0x11100, TLS_DATA = 0x11200, PLAYER = 0x12000,
    LOADED = 0x12700, ROOT = 0x12800, OTHER_ROOT = 0x12900, CHILD = 0x12a00,
    ROOT_CHILDREN = 0x13000, BONE_CHILDREN = 0x13100, FLAT_RECORD = 0x13400, EXTRA_NODES = 0x14000, STACK = 0x1d000,
    ACTOR_TABLE = 0x10d201c, NODE_TABLE = 0x11171a4, FLAT_TABLE = 0x11186a4,
    STOCK_NAMES = 0x12e2fc0, DELTA = 0x12e355c, TLS_INDEX = 0x1bbeb54,
    GET_ROOT = 0x735080, UPDATE = 0xaad9a0, UPDATE_FLAT = 0xabdf60, REFRESH = 0xab53e0,
    ROOT_CALL = 0xaaf336, CHILD_FIRST = 0xaada3f, CHILD_OTHER = 0xaadabf,
    WORLD_DATA_CALL = 0xaad9e1, FULL_MASK = 31, RIGHT_CALF = 16, LAST_NODE = 4,
    BONE_VISIT_CAP = 128, SNAPSHOT_SIZE = 0x1d040, IMAGE_LIMIT = 0x1bc0000
};

static const uint32_t name_slots[] = {4, 0x1c, 0x20, 0x24, 0x28};

typedef struct {
    const CPU *owner;
    uint32_t receiver;
    unsigned visits, named, flattened_only;
    bool member, ambiguous;
} Witness;

typedef enum {
    COMPLETE, FLAT_ATTACHED, MISSING, FLAT_ONLY, WRONG_PARENT, DUPLICATE, OVER_BUDGET, PREVIEW_ROOT, UNLOADED,
    DEGENERATE_POSE
} Scenario;

uint8_t *MEM;
static _Thread_local Witness *witness;

void fixture_update_third_person(CPU *cpu);

#ifdef SKYRIM_TEST_SCENE_RUNTIME
const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t version, uint32_t table_bytes);
static CPU *runtime_cpu;
struct CPU *rt_current_cpu(void) { return runtime_cpu; }
#endif

static unsigned named_bit(uint32_t name)
{
    for (unsigned i = 0; i < sizeof name_slots / sizeof name_slots[0]; ++i)
        if (name && M32(STOCK_NAMES + name_slots[i]) == name) return 1u << i;
    return 0;
}

void fixture_existing_flat_bone(const CPU *cpu, uint32_t tree, uint32_t record, uint32_t node)
{
    if (!witness || witness->owner != cpu || !witness->member || witness->visits > BONE_VISIT_CAP ||
        tree != witness->receiver ||
        tree != M32(LOADED + 0x20) || M32(tree) != FLAT_TABLE || M32(record + 0x70) != node)
        return;
    /* The transform input was materialized. This says nothing about the
     * original downward traversal having visited it as a child. */
    witness->flattened_only |= named_bit(M32(record + 0x74)) & ~witness->named;
}

static void original_downward(CPU *cpu, uint32_t call_site, uint32_t target)
{
    Witness *current = witness && witness->owner == cpu ? witness : NULL;
    const uint32_t node = cpu->r[1];
    const uint32_t previous = current ? current->receiver : 0;
    const bool parent_is_member = current && current->member;
    if (current && current->visits <= BONE_VISIT_CAP) {
        /* Dispatched by the stock root call or the actual child array walk.
         * Only the node already handed to the original call is inspected. */
        const bool room = current->visits++ < BONE_VISIT_CAP;
        const bool root = room && call_site == ROOT_CALL && !previous && node == M32(LOADED + 0x20);
        const bool child = room && (call_site == CHILD_FIRST || call_site == CHILD_OTHER) &&
            parent_is_member && cpu->r[6] == previous && M32(node + 0x18) == previous;
        current->member = root || child;
    }
    if (current) current->receiver = node;
    if (target == UPDATE_FLAT)
        f_ABDF60(cpu);
    else
        f_AAD9A0(cpu);
    if (current) {
        current->receiver = previous;
        current->member = parent_is_member;
    }
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    const uint32_t stack = cpu->r[4], site = M32(stack);
    switch (target) {
    case GET_ROOT:
        assert(site == 0x734fcf && cpu->r[1] == PLAYER && M32(stack + 4) == 0);
        f_735080(cpu);
        break;
    case UPDATE:
    case UPDATE_FLAT:
        assert(site == ROOT_CALL || site == CHILD_FIRST || site == CHILD_OTHER);
        assert(M32(M32(stack + 4)) == M32(DELTA) && M32(M32(stack + 4) + 4) == 0x2001);
        original_downward(cpu, site, target);
        break;
    case REFRESH: {
        assert(site == WORLD_DATA_CALL);
        const uint32_t node = cpu->r[1];
        f_AB53E0(cpu);
        if (witness && witness->owner == cpu && witness->member && witness->visits <= BONE_VISIT_CAP &&
            witness->receiver == node) {
            const unsigned bit = named_bit(M32(node + 8));
            witness->ambiguous |= (witness->named & bit) != 0;
            witness->named |= bit;
        }
        break;
    }
    default: abort();
    }
}

void rt_call(CPU *cpu, uint32_t target) { (void)cpu; (void)target; abort(); }
void f_AC4970(CPU *cpu) { (void)cpu; abort(); } /* Empty authored bounds skip nonzero-sphere merge. */

static void make_node(uint32_t node, uint32_t name, uint32_t parent)
{
    M32(node) = NODE_TABLE;
    M32(node + 4) = 2;
    M32(node + 8) = name;
    M32(node + 0x18) = parent;
    MF32(node + 0x20) = MF32(node + 0x30) = MF32(node + 0x40) = 1.0f;
    MF32(node + 0x44) = MF32(node + 0x50) = 1.0f;
}

typedef struct {
    CPU cpu;
    uint8_t memory[SNAPSHOT_SIZE];
} Result;

static Result run(Scenario mode, bool observe)
{
    memset(MEM, 0, SNAPSHOT_SIZE);
    M32(TLS + 0x2c) = TLS_TABLE;
    M32(TLS_TABLE) = TLS_DATA;
    M32(PLAYER) = ACTOR_TABLE;
    M32(PLAYER + 0x44) = LOADED;
    M32(LOADED + 0x20) = mode == UNLOADED ? 0 : ROOT;
    make_node(ROOT, 0, 0);
    make_node(OTHER_ROOT, 0, 0);
    M32(ROOT + 0xac) = M32(OTHER_ROOT + 0xac) = ROOT_CHILDREN;
    M16(ROOT + 0xb2) = M16(OTHER_ROOT + 0xb2) = 1;
    M32(ROOT_CHILDREN) = CHILD;
    const uint32_t calf = CHILD + LAST_NODE * 0x100;
    for (unsigned i = 0; i <= LAST_NODE; ++i) {
        const uint32_t node = CHILD + i * 0x100;
        const uint32_t name = M32(STOCK_NAMES + name_slots[i]);
        make_node(node, name, i ? CHILD : ROOT);
        if (i >= 3 && mode != DEGENERATE_POSE) MF32(node + 0x4c) = 30.0f;
        if (i) M32(BONE_CHILDREN + (i - 1) * 4) = node;
    }
    M32(CHILD + 0xac) = BONE_CHILDREN;
    M16(CHILD + 0xb2) = LAST_NODE;
    if (mode == MISSING || mode == FLAT_ONLY) M32(BONE_CHILDREN + (LAST_NODE - 1) * 4) = 0;
    if (mode == WRONG_PARENT) M32(calf + 0x18) = OTHER_ROOT;
    if (mode == DUPLICATE) M32(calf + 8) = M32(STOCK_NAMES + name_slots[0]);
    if (mode == OVER_BUDGET) {
        for (unsigned i = LAST_NODE; i < BONE_VISIT_CAP; ++i) {
            const uint32_t extra = EXTRA_NODES + (i - LAST_NODE) * 0xc0;
            make_node(extra, 0, CHILD);
            M32(BONE_CHILDREN + i * 4) = extra;
        }
        M16(CHILD + 0xb2) = BONE_VISIT_CAP;
    }
    if (mode == PREVIEW_ROOT) {
        M32(TLS_DATA + 0x2f4) = PLAYER;
        M32(TLS_DATA + 0x2f0) = OTHER_ROOT;
    }
    if (mode == FLAT_ONLY || mode == FLAT_ATTACHED) {
        M32(ROOT) = FLAT_TABLE;
        M32(ROOT + 0xb8) = 1;
        M32(ROOT + 0xc0) = FLAT_RECORD;
        M32(FLAT_RECORD + 0x70) = calf;
        M32(FLAT_RECORD + 0x74) = M32(calf + 8);
        MF32(calf + 0x78) = 25.0f;
    }

    CPU cpu = {.r = {1, PLAYER, 3, 4, STACK, 6, PLAYER, 8}, .fs_base = TLS};
    Witness sample = {.owner = &cpu};
    witness = observe ? &sample : NULL;
#ifdef SKYRIM_TEST_SCENE_RUNTIME
    runtime_cpu = &cpu;
#endif
    fixture_update_third_person(&cpu);
#ifdef SKYRIM_TEST_SCENE_RUNTIME
    runtime_cpu = NULL;
#endif
    witness = NULL;
    assert(cpu.r[4] == STACK && !sample.receiver && !sample.member);
    const bool complete = sample.named == FULL_MASK && !sample.ambiguous && sample.visits <= BONE_VISIT_CAP;
    const bool all_nodes_present = mode == COMPLETE || mode == FLAT_ATTACHED || mode == DEGENERATE_POSE;
    assert(complete == (observe && all_nodes_present));
    if (observe) {
        const unsigned expected = all_nodes_present || mode == OVER_BUDGET ? FULL_MASK :
            (mode == UNLOADED || mode == PREVIEW_ROOT ? 0 : FULL_MASK & ~RIGHT_CALF);
        assert(sample.named == expected && sample.ambiguous == (mode == DUPLICATE));
        assert(sample.flattened_only == (mode == FLAT_ONLY ? RIGHT_CALF : 0));
        assert((sample.visits > BONE_VISIT_CAP) == (mode == OVER_BUDGET));
    }
    if (mode == FLAT_ONLY || mode == FLAT_ATTACHED) {
        assert(MF32(FLAT_RECORD + 0x58) == MF32(calf + 0x78));
        assert(MF32(FLAT_RECORD + 0x58) == (mode == FLAT_ONLY ? 25.0f : 3.0f));
    }
    assert(M32(ROOT + 4) == 2 && M32(CHILD + 4) == 2 && M32(calf + 4) == 2);
    Result final = {.cpu = cpu};
    memcpy(final.memory, MEM, sizeof final.memory);
    return final;
}

static void equal_results(const Result *ordinary, const Result *observed)
{
    assert(!memcmp(ordinary->cpu.r, observed->cpu.r, sizeof ordinary->cpu.r));
    assert(!memcmp(&ordinary->cpu.flags, &observed->cpu.flags, sizeof ordinary->cpu.flags));
    assert(ordinary->cpu.top == observed->cpu.top && ordinary->cpu.fs_base == observed->cpu.fs_base);
    assert(!memcmp(ordinary->memory, observed->memory, sizeof ordinary->memory));
}

int main(int argc, char **argv)
{
    MEM = calloc(IMAGE_LIMIT, 1);
    assert(MEM && !M32(TLS_INDEX));
    M32(ACTOR_TABLE + 0x1bc) = GET_ROOT;
    M32(NODE_TABLE + 0xa4) = UPDATE;
    M32(FLAT_TABLE + 0xa4) = UPDATE_FLAT;
    M32(NODE_TABLE + 0xb4) = M32(FLAT_TABLE + 0xb4) = REFRESH;
    M32(DELTA) = 0x3c888889;
    for (unsigned i = 0; i <= LAST_NODE; ++i)
        M32(STOCK_NAMES + name_slots[i]) = 0x13800 + i * 8;
#ifdef SKYRIM_TEST_SCENE_RUNTIME
    assert(argc == 2 && strlen(argv[1]) == 1);
    const int mode = argv[1][0] - '0';
    assert(mode >= COMPLETE && mode <= DEGENERATE_POSE);
    const Result ordinary = run((Scenario)mode, false);
    PcGuestModHooks host = {.version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof host,
                            .groups = PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE};
    const PcGuestModHooksGuest *guest = skyrim_guest_bind_mod_hooks_v1(PC_GUEST_MOD_HOOKS_ABI, sizeof host);
    skyrim_highheel_scene_fill(&host);
    assert(guest && guest->compiled_groups == host.groups && guest->select(&host) == 0);
    const Result observed = run((Scenario)mode, true);
    equal_results(&ordinary, &observed);
    /* A completed stock update gets only one line across the launch. */
    const Result repeated = run((Scenario)mode, true);
    equal_results(&ordinary, &repeated);
#else
    assert(argc == 1);
    (void)argv;
    for (Scenario mode = COMPLETE; mode <= DEGENERATE_POSE; ++mode) {
        const Result ordinary = run(mode, false), observed = run(mode, true);
        equal_results(&ordinary, &observed);
    }
#endif
    free(MEM);
    puts("Classic third-person traversal witnesses five actual named nodes without changing the scene");
}
